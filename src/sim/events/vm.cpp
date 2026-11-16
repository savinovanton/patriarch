#include "pat/sim/events/vm.hpp"

#include "pat/core/bytes.hpp"
#include "pat/core/detail/wide_math.hpp"
#include "pat/core/hash_stream.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

namespace pat::sim::events {

namespace {

constexpr Word kWordMax = std::numeric_limits<Word>::max();
constexpr Word kWordMin = std::numeric_limits<Word>::min();
constexpr std::uint32_t kPpmOne = 1'000'000;

// ---- saturating arithmetic (section 7.3: overflow saturates and is counted, never undefined) ----

[[nodiscard]] constexpr std::uint64_t abs_u64(Word v) noexcept {
    return v < 0 ? (0ULL - static_cast<std::uint64_t>(v)) : static_cast<std::uint64_t>(v);
}

/// Magnitude + sign to a word; saturates (and reports it) when it does not fit.
[[nodiscard]] constexpr Word from_magnitude(std::uint64_t mag, bool negative, bool& saturated) noexcept {
    const std::uint64_t limit = static_cast<std::uint64_t>(kWordMax) + (negative ? 1ULL : 0ULL);
    if (mag > limit) {
        saturated = true;
        return negative ? kWordMin : kWordMax;
    }
    return negative ? static_cast<Word>(0ULL - mag) : static_cast<Word>(mag);
}

[[nodiscard]] Word add_sat(Word a, Word b, RunCounters& c) noexcept {
    const Word r = static_cast<Word>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
    if (((a ^ r) & (b ^ r)) < 0) {
        ++c.saturations;
        return a < 0 ? kWordMin : kWordMax;
    }
    return r;
}

[[nodiscard]] Word sub_sat(Word a, Word b, RunCounters& c) noexcept {
    const Word r = static_cast<Word>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
    if (((a ^ b) & (a ^ r)) < 0) {
        ++c.saturations;
        return a < 0 ? kWordMin : kWordMax;
    }
    return r;
}

[[nodiscard]] Word mul_sat(Word a, Word b, RunCounters& c) noexcept {
    const bool negative = (a < 0) != (b < 0);
    const core::detail::U128 shifted =
        core::detail::shr(core::detail::mul_u64(abs_u64(a), abs_u64(b)), core::Fixed64::frac_bits);
    bool saturated = shifted.hi != 0;
    const Word r =
        saturated ? (negative ? kWordMin : kWordMax) : from_magnitude(shifted.lo, negative, saturated);
    if (saturated) {
        ++c.saturations;
    }
    return r;
}

[[nodiscard]] Word div_sat(Word a, Word b, RunCounters& c) noexcept {
    if (b == 0) {
        ++c.div_zero;
        return 0;
    }
    const bool negative = (a < 0) != (b < 0);
    const core::detail::U128 numerator =
        core::detail::shl(core::detail::U128{0, abs_u64(a)}, core::Fixed64::frac_bits);
    bool overflow = false;
    const std::uint64_t q = core::detail::div_u128_by_u64(numerator, abs_u64(b), overflow);
    bool saturated = overflow;
    const Word r = overflow ? (negative ? kWordMin : kWordMax) : from_magnitude(q, negative, saturated);
    if (saturated) {
        ++c.saturations;
    }
    return r;
}

[[nodiscard]] Word neg_sat(Word a, RunCounters& c) noexcept {
    if (a == kWordMin) {
        ++c.saturations;
        return kWordMax;
    }
    return -a;
}

[[nodiscard]] constexpr Word trunc_whole(Word a) noexcept {
    return (a / core::Fixed64::one_raw) * core::Fixed64::one_raw;
}

// ---- weighted choice (RAND_PICK, ITER_PICK, Preview shares) ----------------------------------------

/// Scales non-negative weights down so that their sum fits in 32 bits. Returns the sum (0: no
/// positive weight). The scaling depends only on the weights and their count, so shares and rolls
/// are the same on every platform.
[[nodiscard]] std::uint64_t scale_weights(std::span<const Word> weights,
                                          std::span<std::uint64_t> scaled) noexcept {
    std::uint64_t max_w = 0;
    for (const Word w : weights) {
        max_w = std::max<std::uint64_t>(max_w, w > 0 ? static_cast<std::uint64_t>(w) : 0ULL);
    }
    if (max_w == 0) {
        std::fill(scaled.begin(), scaled.end(), 0ULL);
        return 0;
    }
    const int k = std::bit_width(static_cast<std::uint64_t>(weights.size()));
    const int m = std::bit_width(max_w);
    const int shift = std::max(0, k + m - 32);
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < weights.size(); ++i) {
        scaled[i] = (weights[i] > 0 ? static_cast<std::uint64_t>(weights[i]) : 0ULL) >> shift;
        total += scaled[i];
    }
    return total;
}

/// Index of the entry hit by `r` in [0, total), by cumulative scaled weight.
[[nodiscard]] std::int32_t pick_by_roll(std::span<const std::uint64_t> scaled, std::uint64_t r) noexcept {
    std::uint64_t acc = 0;
    for (std::size_t i = 0; i < scaled.size(); ++i) {
        acc += scaled[i];
        if (r < acc) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

[[nodiscard]] std::int32_t first_positive(std::span<const Word> weights) noexcept {
    for (std::size_t i = 0; i < weights.size(); ++i) {
        if (weights[i] > 0) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

// ---- the executor -----------------------------------------------------------------------------------

template <Mode M>
struct Exec {
    const Program& prog;
    VmHost& host;
    const RunOptions& opt;
    Vm::Scratch& sc;
    RunResult& res;

    std::array<Word, limits::kMaxStack> stack{};
    std::size_t sp = 0;
    std::array<Scope, limits::kMaxScopeDepth> scopes{};
    std::size_t sd = 0;
    std::array<Scope, limits::kMaxSavedScopes> saved{};

    struct Frame {
        std::uint32_t base = 0;
        std::uint32_t count = 0;
        std::uint32_t cursor = 0;
        std::size_t elem_depth = 0;
        bool pushed = false;
    };
    std::array<Frame, limits::kMaxIterNest> frames{};
    std::size_t nf = 0;

    struct Fork {
        std::uint32_t table_pc = 0;
        std::int32_t table_b = 0;
        std::uint16_t n = 0;
        std::int32_t cur = 0;
        bool chance = false;
        std::array<std::uint32_t, limits::kMaxBranches> shares{};
        std::size_t sp = 0;
        std::size_t sd = 0;
        std::size_t nf = 0;
    };
    std::array<Fork, limits::kMaxForkDepth> forks{};
    std::size_t nforks = 0;
    std::array<std::uint32_t, limits::kMaxBranches> pending_shares{};
    std::uint16_t pending_n = 0;

    int hidden = 0;
    std::uint32_t emitted = 0;
    std::uint32_t state_ops = 0;
    std::uint32_t cur = 0;
    std::uint32_t next = 0;

    Exec(const Program& p, VmHost& h, const RunOptions& o, Vm::Scratch& s, RunResult& r)
        : prog(p), host(h), opt(o), sc(s), res(r) {}

    // ---- failure and stack helpers ----

    bool fail(AbortReason reason, std::uint32_t detail = 0) {
        res.ok = false;
        res.reason = reason;
        res.abort_pc = cur;
        res.abort_detail = detail;
        return false;
    }

    bool push(Word w) {
        if (sp >= limits::kMaxStack) {
            return fail(AbortReason::Stack);
        }
        stack[sp++] = w;
        return true;
    }

    bool pop(Word& w) {
        if (sp == 0) {
            return fail(AbortReason::BadProgram);
        }
        w = stack[--sp];
        return true;
    }

    bool push_scope(Scope s) {
        if (sd >= limits::kMaxScopeDepth) {
            return fail(AbortReason::ScopeDepth);
        }
        scopes[sd++] = s;
        return true;
    }

    [[nodiscard]] Scope cur_scope() const noexcept { return scopes[sd - 1]; }

    bool jump_abs(std::int64_t target) {
        if (target < 0 || target >= static_cast<std::int64_t>(prog.code.size())) {
            return fail(AbortReason::BadProgram);
        }
        next = static_cast<std::uint32_t>(target);
        return true;
    }

    bool jump(std::int32_t b) { return jump_abs(static_cast<std::int64_t>(cur) + 1 + b); }

    // ---- iterator helpers ----

    Frame* top_frame() {
        if (nf == 0) {
            fail(AbortReason::BadProgram);
            return nullptr;
        }
        return &frames[nf - 1];
    }

    /// Removes the element scope of a frame (if one is pushed) and puts the cursor at the start.
    bool rewind(Frame& f) {
        if (f.pushed) {
            if (sd != f.elem_depth) {
                return fail(AbortReason::BadProgram);
            }
            --sd;
            f.pushed = false;
        }
        f.cursor = 0;
        return true;
    }

    // ---- preview helpers ----

    bool open_fork(Fork fork) {
        if (nforks >= limits::kMaxForkDepth) {
            return fail(AbortReason::ForkDepth);
        }
        fork.sp = sp;
        fork.sd = sd;
        fork.nf = nf;
        forks[nforks++] = fork;
        return true;
    }

    void close_preview_groups() {
        if constexpr (M == Mode::Preview) {
            if (opt.preview != nullptr) {
                for (std::size_t i = 0; i < nf + nforks; ++i) {
                    opt.preview->group_end();
                }
            }
        }
    }

    void leaf(Op op, std::uint16_t code, bool result, Word v0, Word v1) {
        if constexpr (M == Mode::Trace) {
            if (opt.tracer != nullptr) {
                opt.tracer->leaf(TraceLeaf{cur, op, code, result, v0, v1});
            }
        } else {
            (void)op;
            (void)code;
            (void)result;
            (void)v0;
            (void)v1;
        }
    }

    // ---- the loop ----

    bool run() {
        scopes[0] = opt.root;
        sd = 1;
        std::uint32_t max_instr = opt.max_instr;
        if (max_instr == 0) {
            max_instr = opt.kind == Kind::Effect ? limits::kMaxEffectInstr : limits::kMaxTriggerInstr;
        }
        const std::size_t n = prog.code.size();
        for (;;) {
            cur = next;
            if (cur >= n) {
                return fail(AbortReason::BadProgram);
            }
            next = cur + 1;
            if (res.instructions >= max_instr) {
                return fail(AbortReason::Instr);
            }
            ++res.instructions;
            const Instr in = prog.code[cur];
            const Op op = static_cast<Op>(in.op);
            if (op == Op::kLoad || op == Op::kPred) {
                res.wu += host.cost(op, in.a);
            } else {
                res.wu += op_base_wu(in.op);
            }

            switch (op) {
                case Op::kNop:
                    break;
                case Op::kPushInt:
                    if (!push(word_from_int(in.b))) {
                        return false;
                    }
                    break;
                case Op::kPushConst:
                    if (in.b < 0 || static_cast<std::size_t>(in.b) >= prog.consts.size()) {
                        return fail(AbortReason::BadProgram);
                    }
                    if (!push(prog.consts[static_cast<std::size_t>(in.b)])) {
                        return false;
                    }
                    break;
                case Op::kPushBool:
                    if (!push(in.b != 0 ? 1 : 0)) {
                        return false;
                    }
                    break;
                case Op::kDup: {
                    Word v = 0;
                    if (!pop(v) || !push(v) || !push(v)) {
                        return false;
                    }
                    break;
                }
                case Op::kDrop: {
                    Word v = 0;
                    if (!pop(v)) {
                        return false;
                    }
                    break;
                }
                case Op::kSwap: {
                    Word b = 0;
                    Word a = 0;
                    if (!pop(b) || !pop(a) || !push(b) || !push(a)) {
                        return false;
                    }
                    break;
                }
                case Op::kAdd:
                case Op::kSub:
                case Op::kMul:
                case Op::kDiv:
                case Op::kMin:
                case Op::kMax: {
                    Word b = 0;
                    Word a = 0;
                    if (!pop(b) || !pop(a)) {
                        return false;
                    }
                    Word r = 0;
                    switch (op) {
                        case Op::kAdd:
                            r = add_sat(a, b, res.counters);
                            break;
                        case Op::kSub:
                            r = sub_sat(a, b, res.counters);
                            break;
                        case Op::kMul:
                            r = mul_sat(a, b, res.counters);
                            break;
                        case Op::kDiv:
                            r = div_sat(a, b, res.counters);
                            break;
                        case Op::kMin:
                            r = std::min(a, b);
                            break;
                        default:
                            r = std::max(a, b);
                            break;
                    }
                    if (!push(r)) {
                        return false;
                    }
                    break;
                }
                case Op::kNeg:
                case Op::kTrunc:
                case Op::kNot: {
                    Word a = 0;
                    if (!pop(a)) {
                        return false;
                    }
                    const Word r = op == Op::kNeg ? neg_sat(a, res.counters)
                                                  : (op == Op::kTrunc ? trunc_whole(a) : (a == 0 ? 1 : 0));
                    if (!push(r)) {
                        return false;
                    }
                    break;
                }
                case Op::kClamp: {
                    Word hi = 0;
                    Word lo = 0;
                    Word x = 0;
                    if (!pop(hi) || !pop(lo) || !pop(x)) {
                        return false;
                    }
                    // hi < lo gives lo: the lower bound wins.
                    if (!push(std::max(lo, std::min(x, hi)))) {
                        return false;
                    }
                    break;
                }
                case Op::kEq:
                case Op::kNe:
                case Op::kLt:
                case Op::kLe:
                case Op::kGt:
                case Op::kGe: {
                    Word r = 0;
                    Word l = 0;
                    if (!pop(r) || !pop(l)) {
                        return false;
                    }
                    bool result = false;
                    switch (op) {
                        case Op::kEq:
                            result = l == r;
                            break;
                        case Op::kNe:
                            result = l != r;
                            break;
                        case Op::kLt:
                            result = l < r;
                            break;
                        case Op::kLe:
                            result = l <= r;
                            break;
                        case Op::kGt:
                            result = l > r;
                            break;
                        default:
                            result = l >= r;
                            break;
                    }
                    leaf(op, 0, result, l, r);
                    if (!push(result ? 1 : 0)) {
                        return false;
                    }
                    break;
                }
                case Op::kJmp:
                    if (!jump(in.b)) {
                        return false;
                    }
                    break;
                case Op::kJf:
                case Op::kJt: {
                    Word v = 0;
                    if (!pop(v)) {
                        return false;
                    }
                    if ((v == 0) == (op == Op::kJf) && !jump(in.b)) {
                        return false;
                    }
                    break;
                }
                case Op::kJfKeep:
                case Op::kJtKeep: {
                    if (sp == 0) {
                        return fail(AbortReason::BadProgram);
                    }
                    const Word v = stack[sp - 1];
                    if ((v == 0) == (op == Op::kJfKeep) && !jump(in.b)) {
                        return false;
                    }
                    break;
                }

                // ---- scopes ----
                case Op::kScopeEnter: {
                    const Scope s = host.transition(cur_scope(), in.a);
                    if (s.empty()) {
                        if (!jump(in.b)) {
                            return false;
                        }
                    } else if (!push_scope(s)) {
                        return false;
                    }
                    break;
                }
                case Op::kScopeLeave:
                    if (sd <= 1) {
                        return fail(AbortReason::BadProgram);
                    }
                    --sd;
                    break;
                case Op::kScopeSave:
                    if (in.a >= limits::kMaxSavedScopes) {
                        return fail(AbortReason::BadProgram);
                    }
                    saved[in.a] = cur_scope();
                    break;
                case Op::kScopeLoad: {
                    if (in.a >= limits::kMaxSavedScopes) {
                        return fail(AbortReason::BadProgram);
                    }
                    const Scope s = saved[in.a];
                    if (s.empty()) {
                        if (!jump(in.b)) {
                            return false;
                        }
                    } else if (!push_scope(s)) {
                        return false;
                    }
                    break;
                }
                case Op::kScopeRoot:
                    if (!push_scope(opt.root)) {
                        return false;
                    }
                    break;

                // ---- host reads ----
                case Op::kLoad:
                case Op::kPred: {
                    if (in.b < 0 || static_cast<std::size_t>(in.b) > limits::kMaxEffectArgs) {
                        return fail(AbortReason::BadProgram);
                    }
                    std::array<Word, limits::kMaxEffectArgs> args{};
                    const auto argc = static_cast<std::size_t>(in.b);
                    for (std::size_t i = argc; i-- > 0;) {
                        if (!pop(args[i])) {
                            return false;
                        }
                    }
                    const std::span<const Word> view(args.data(), argc);
                    if (op == Op::kLoad) {
                        if (!push(host.load(cur_scope(), in.a, view))) {
                            return false;
                        }
                    } else {
                        const bool result = host.predicate(cur_scope(), in.a, view);
                        leaf(op, in.a, result, argc > 0 ? args[0] : 0, 0);
                        if (!push(result ? 1 : 0)) {
                            return false;
                        }
                    }
                    break;
                }
                case Op::kLoadVar:
                    if (in.a > 1 || in.b < 0 || static_cast<std::size_t>(in.b) >= prog.consts.size()) {
                        return fail(AbortReason::BadProgram);
                    }
                    if (!push(host.load_var(
                            cur_scope(),
                            in.a != 0,
                            static_cast<std::uint64_t>(prog.consts[static_cast<std::size_t>(in.b)])))) {
                        return false;
                    }
                    break;

                // ---- iterators ----
                case Op::kIterBegin: {
                    if (nf >= limits::kMaxIterNest) {
                        return fail(AbortReason::IterNest);
                    }
                    sc.listing.clear();
                    bool cut = host.list(
                        cur_scope(), in.a, static_cast<std::uint32_t>(limits::kMaxFanout), sc.listing);
                    std::size_t count = sc.listing.size();
                    if (count > limits::kMaxFanout) {
                        count = limits::kMaxFanout;
                        cut = true;
                    }
                    if (cut) {
                        ++res.counters.truncated;
                    }
                    Frame f;
                    f.base = static_cast<std::uint32_t>(sc.items.size());
                    f.count = static_cast<std::uint32_t>(count);
                    for (std::size_t i = 0; i < count; ++i) {
                        sc.items.push_back(Vm::IterElem{sc.listing[i], core::Fixed64::one_raw, true});
                    }
                    frames[nf++] = f;
                    res.wu += static_cast<std::uint32_t>(count);
                    if constexpr (M == Mode::Preview) {
                        if (opt.preview != nullptr) {
                            opt.preview->group_begin(
                                PreviewGroup::Each, in.a, static_cast<std::uint32_t>(count));
                        }
                    }
                    break;
                }
                case Op::kIterNext: {
                    Frame* f = top_frame();
                    if (f == nullptr || !rewind_keep_cursor(*f)) {
                        return false;
                    }
                    if (f->cursor < f->count) {
                        const Scope s = sc.items[f->base + f->cursor].scope;
                        if (!push_scope(s)) {
                            return false;
                        }
                        f->pushed = true;
                        f->elem_depth = sd;
                        ++f->cursor;
                        if constexpr (M == Mode::Preview) {
                            if (opt.preview != nullptr) {
                                opt.preview->element(s);
                            }
                        }
                    } else if (!jump(in.b)) {
                        return false;
                    }
                    break;
                }
                case Op::kIterEnd: {
                    Frame* f = top_frame();
                    if (f == nullptr) {
                        return false;
                    }
                    if (f->pushed) {
                        if (sd != f->elem_depth) {
                            return fail(AbortReason::BadProgram);
                        }
                        --sd;
                    }
                    sc.items.resize(f->base);
                    --nf;
                    if constexpr (M == Mode::Preview) {
                        if (opt.preview != nullptr) {
                            opt.preview->group_end();
                        }
                    }
                    break;
                }
                case Op::kIterOrder: {
                    Frame* f = top_frame();
                    if (f == nullptr || !rewind(*f)) {
                        return false;
                    }
                    sc.keyed.clear();
                    for (std::uint32_t i = 0; i < f->count; ++i) {
                        const Vm::IterElem& e = sc.items[f->base + i];
                        sc.keyed.push_back(Vm::Keyed{host.load(e.scope, in.a, {}), e});
                    }
                    res.wu += f->count * (2 + host.cost(Op::kLoad, in.a));
                    std::stable_sort(
                        sc.keyed.begin(), sc.keyed.end(), [](const Vm::Keyed& x, const Vm::Keyed& y) {
                            if (x.key != y.key) {
                                return x.key > y.key;
                            }
                            return x.elem.scope.word() < y.elem.scope.word();
                        });
                    for (std::uint32_t i = 0; i < f->count; ++i) {
                        sc.items[f->base + i] = sc.keyed[i].elem;
                    }
                    break;
                }
                case Op::kIterTake: {
                    Frame* f = top_frame();
                    if (f == nullptr || !rewind(*f)) {
                        return false;
                    }
                    const auto keep = static_cast<std::uint32_t>(std::max(in.b, 0));
                    if (keep < f->count) {
                        f->count = keep;
                        sc.items.resize(f->base + keep);
                    }
                    break;
                }
                case Op::kIterKeep:
                case Op::kIterWeight: {
                    Frame* f = top_frame();
                    Word v = 0;
                    if (f == nullptr || !pop(v)) {
                        return false;
                    }
                    if (!f->pushed) {
                        return fail(AbortReason::BadProgram);
                    }
                    Vm::IterElem& e = sc.items[f->base + f->cursor - 1];
                    if (op == Op::kIterKeep) {
                        e.keep = v != 0;
                    } else {
                        e.weight = v;
                    }
                    break;
                }
                case Op::kIterRewind: {
                    Frame* f = top_frame();
                    if (f == nullptr || !rewind(*f)) {
                        return false;
                    }
                    std::uint32_t kept = 0;
                    for (std::uint32_t i = 0; i < f->count; ++i) {
                        if (sc.items[f->base + i].keep) {
                            sc.items[f->base + kept] = sc.items[f->base + i];
                            sc.items[f->base + kept].keep = true;
                            ++kept;
                        }
                    }
                    f->count = kept;
                    sc.items.resize(f->base + kept);
                    break;
                }
                case Op::kIterPick: {
                    Frame* f = top_frame();
                    if (f == nullptr || !rewind(*f)) {
                        return false;
                    }
                    res.wu += f->count;
                    std::array<Word, limits::kMaxFanout> weights{};
                    std::array<std::uint64_t, limits::kMaxFanout> scaled{};
                    (void)scaled;
                    for (std::uint32_t i = 0; i < f->count; ++i) {
                        weights[i] = sc.items[f->base + i].weight;
                    }
                    const std::span<const Word> w(weights.data(), f->count);
                    std::int32_t index = -1;
                    if constexpr (M == Mode::Run) {
                        const std::uint64_t total =
                            scale_weights(w, std::span<std::uint64_t>(scaled.data(), f->count));
                        if (total > 0) {
                            ++res.counters.draws;
                            const std::uint64_t r = host.rng().bounded(static_cast<std::uint32_t>(total));
                            index = pick_by_roll(std::span<const std::uint64_t>(scaled.data(), f->count), r);
                        }
                    } else {
                        index = first_positive(w);
                        if constexpr (M == Mode::Preview) {
                            if (opt.preview != nullptr) {
                                std::uint32_t positive = 0;
                                for (const Word x : w) {
                                    positive += x > 0 ? 1U : 0U;
                                }
                                opt.preview->iter_pick(positive);
                            }
                        }
                    }
                    if (index >= 0) {
                        sc.items[f->base] = sc.items[f->base + static_cast<std::uint32_t>(index)];
                        f->count = 1;
                    } else {
                        f->count = 0;
                    }
                    sc.items.resize(f->base + f->count);
                    break;
                }

                // ---- randomness ----
                case Op::kRandPermille: {
                    Word w = 0;
                    if (!pop(w)) {
                        return false;
                    }
                    const std::int64_t ppm = fixed_from_word(w).to_int();
                    if constexpr (M == Mode::Run) {
                        ++res.counters.draws;
                        const std::uint32_t roll = host.rng().bounded(kPpmOne);
                        if (!push(static_cast<std::int64_t>(roll) < ppm ? 1 : 0)) {
                            return false;
                        }
                    } else if constexpr (M == Mode::Trace) {
                        leaf(op, 0, true, ppm, 0);
                        if (!push(1)) {
                            return false;
                        }
                    } else {
                        if (in.b != 0) {
                            if (ppm <= 0) {
                                if (!push(0)) {
                                    return false;
                                }
                            } else {
                                Fork fork;
                                fork.chance = true;
                                if (!open_fork(fork) || !push(1)) {
                                    return false;
                                }
                                if (opt.preview != nullptr) {
                                    opt.preview->group_begin(
                                        PreviewGroup::Chance,
                                        0,
                                        static_cast<std::uint32_t>(std::min<std::int64_t>(ppm, kPpmOne)));
                                }
                            }
                        } else if (!push(1)) {
                            return false;
                        }
                    }
                    break;
                }
                case Op::kRandPick: {
                    if (in.a < 1 || in.a > limits::kMaxBranches) {
                        return fail(AbortReason::BadProgram);
                    }
                    const std::size_t count = in.a;
                    std::array<Word, limits::kMaxBranches> weights{};
                    for (std::size_t i = count; i-- > 0;) {
                        if (!pop(weights[i])) {
                            return false;
                        }
                    }
                    const std::span<const Word> w(weights.data(), count);
                    std::array<std::uint64_t, limits::kMaxBranches> scaled{};
                    const std::span<std::uint64_t> scaled_view(scaled.data(), count);
                    const std::uint64_t total = scale_weights(w, scaled_view);
                    std::int32_t index = -1;
                    if constexpr (M == Mode::Run) {
                        if (total > 0) {
                            ++res.counters.draws;
                            const std::uint64_t r = host.rng().bounded(static_cast<std::uint32_t>(total));
                            index = pick_by_roll(scaled_view, r);
                        }
                    } else {
                        index = first_positive(w);
                        if constexpr (M == Mode::Preview) {
                            // Shares of 1 000 000 that add up exactly; the last branch takes the remainder.
                            pending_shares.fill(0);
                            pending_n = in.a;
                            std::uint32_t given = 0;
                            std::int32_t last = -1;
                            for (std::size_t i = 0; i < count; ++i) {
                                if (scaled[i] > 0) {
                                    pending_shares[i] =
                                        static_cast<std::uint32_t>(scaled[i] * kPpmOne / total);
                                    given += pending_shares[i];
                                    last = static_cast<std::int32_t>(i);
                                }
                            }
                            if (last >= 0) {
                                pending_shares[static_cast<std::size_t>(last)] += kPpmOne - given;
                            }
                        }
                    }
                    if (!push(word_from_int(index))) {
                        return false;
                    }
                    break;
                }
                case Op::kJTable: {
                    Word w = 0;
                    if (!pop(w)) {
                        return false;
                    }
                    const std::int64_t idx = fixed_from_word(w).to_int();
                    const std::int64_t base = static_cast<std::int64_t>(cur) + 1 + in.b;
                    if constexpr (M == Mode::Preview) {
                        if (pending_n == in.a && idx >= 0 && idx < in.a) {
                            pending_n = 0;
                            Fork fork;
                            fork.table_pc = cur;
                            fork.table_b = in.b;
                            fork.n = in.a;
                            fork.cur = static_cast<std::int32_t>(idx);
                            fork.shares = pending_shares;
                            if (!open_fork(fork)) {
                                return false;
                            }
                            if (opt.preview != nullptr) {
                                opt.preview->group_begin(PreviewGroup::Branch,
                                                         static_cast<std::uint32_t>(idx),
                                                         pending_shares[static_cast<std::size_t>(idx)]);
                            }
                            if (!jump_abs(base + idx)) {
                                return false;
                            }
                            break;
                        }
                        pending_n = 0;
                    }
                    if (!jump_abs(base +
                                  ((idx >= 0 && idx < in.a) ? idx : static_cast<std::int64_t>(in.a)))) {
                        return false;
                    }
                    break;
                }
                case Op::kBranchEnd: {
                    if constexpr (M == Mode::Preview) {
                        if (nforks == 0) {
                            break;
                        }
                        Fork& fork = forks[nforks - 1];
                        if (fork.sp != sp || fork.sd != sd || fork.nf != nf) {
                            return fail(AbortReason::BadProgram);
                        }
                        if (opt.preview != nullptr) {
                            opt.preview->group_end();
                        }
                        std::int32_t following = -1;
                        if (!fork.chance) {
                            for (std::int32_t i = fork.cur + 1; i < fork.n && following < 0; ++i) {
                                if (fork.shares[static_cast<std::size_t>(i)] > 0) {
                                    following = i;
                                }
                            }
                        }
                        if (following >= 0) {
                            fork.cur = following;
                            if (opt.preview != nullptr) {
                                opt.preview->group_begin(PreviewGroup::Branch,
                                                         static_cast<std::uint32_t>(following),
                                                         fork.shares[static_cast<std::size_t>(following)]);
                            }
                            if (!jump_abs(static_cast<std::int64_t>(fork.table_pc) + 1 + fork.table_b +
                                          following)) {
                                return false;
                            }
                            break;
                        }
                        --nforks;
                    }
                    break;
                }
                case Op::kHide:
                    if (in.a == 1) {
                        ++hidden;
                    } else if (hidden > 0) {
                        --hidden;
                    }
                    break;

                // ---- effects ----
                case Op::kEmit:
                case Op::kState: {
                    if (opt.kind != Kind::Effect || in.b < 0 ||
                        static_cast<std::size_t>(in.b) > limits::kMaxEffectArgs) {
                        return fail(AbortReason::BadProgram);
                    }
                    std::array<Word, limits::kMaxEffectArgs> args{};
                    const auto argc = static_cast<std::size_t>(in.b);
                    for (std::size_t i = argc; i-- > 0;) {
                        if (!pop(args[i])) {
                            return false;
                        }
                    }
                    if constexpr (M != Mode::Trace) {
                        const bool is_emit = op == Op::kEmit;
                        std::uint32_t& count = is_emit ? emitted : state_ops;
                        const std::uint32_t limit = is_emit ? opt.max_commands : opt.max_state_ops;
                        if (count >= limit) {
                            return fail(is_emit ? AbortReason::Commands : AbortReason::StateOps);
                        }
                        ++count;
                        const std::span<const Word> view(args.data(), argc);
                        if constexpr (M == Mode::Run) {
                            (is_emit ? res.commands : res.state_ops).add(in.a, cur_scope(), view);
                        } else if (opt.preview != nullptr && hidden == 0) {
                            opt.preview->line(is_emit ? PreviewLineKind::Effect : PreviewLineKind::State,
                                              in.a,
                                              cur_scope(),
                                              view);
                        }
                    }
                    break;
                }

                // ---- end ----
                case Op::kRet:
                    res.ok = true;
                    res.has_value = sp > 0;
                    res.value = sp > 0 ? stack[sp - 1] : 0;
                    close_preview_groups();
                    return true;
                case Op::kAbort:
                    return fail(AbortReason::Explicit, in.a);
                case Op::kStep:
                    if (sp == 0) {
                        return fail(AbortReason::BadProgram);
                    }
                    if (opt.tracer != nullptr) {
                        opt.tracer->step(in.a, stack[sp - 1]);
                    }
                    break;
                default:
                    return fail(AbortReason::BadProgram);
            }
        }
    }

    /// ITER_NEXT first drops the previous element scope but keeps the cursor.
    bool rewind_keep_cursor(Frame& f) {
        if (f.pushed) {
            if (sd != f.elem_depth) {
                return fail(AbortReason::BadProgram);
            }
            --sd;
            f.pushed = false;
        }
        return true;
    }
};

template <Mode M>
RunResult execute(const Program& program, VmHost& host, const RunOptions& options, Vm::Scratch& scratch) {
    RunResult result;
    scratch.items.clear();
    Exec<M> exec(program, host, options, scratch, result);
    const bool ok = exec.run();
    if (!ok) {
        // Rollback: nothing of a failed run is handed to the caller (section 7.4). The random draws
        // already taken stay spent, deliberately: the stream position is a function of the state.
        result.ok = false;
        result.has_value = false;
        result.value = 0;
        result.commands.clear();
        result.state_ops.clear();
    }
    scratch.items.clear();
    return result;
}

void put_buffer(core::ByteWriter& out, const OpBuffer& buffer) {
    out.u32(static_cast<std::uint32_t>(buffer.size()));
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        const OpBuffer::Entry& e = buffer.entry(i);
        out.u32(e.code);
        out.u32(static_cast<std::uint32_t>(e.target.kind));
        out.u32(e.target.id);
        out.u32(e.argc);
        for (const Word w : buffer.args_of(i)) {
            out.i64(w);
        }
    }
}

[[nodiscard]] std::int64_t to_points(Word raw) noexcept {
    const std::int64_t q16 = raw >> 16;  // Q32.32 raw to Q16.16 raw (floor)
    return std::clamp<std::int64_t>(
        q16, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max());
}

}  // namespace

const char* abort_reason_name(AbortReason reason) noexcept {
    switch (reason) {
        case AbortReason::None:
            return "none";
        case AbortReason::Instr:
            return "instr";
        case AbortReason::Stack:
            return "stack";
        case AbortReason::ScopeDepth:
            return "scope_depth";
        case AbortReason::IterNest:
            return "iter_nest";
        case AbortReason::Commands:
            return "commands";
        case AbortReason::StateOps:
            return "state_ops";
        case AbortReason::BadProgram:
            return "bad_program";
        case AbortReason::Explicit:
            return "explicit";
        case AbortReason::ForkDepth:
            return "fork_depth";
    }
    return "?";
}

void OpBuffer::add(std::uint16_t code, Scope target, std::span<const Word> args) {
    entries_.push_back(Entry{
        code, static_cast<std::uint16_t>(args.size()), static_cast<std::uint32_t>(args_.size()), target});
    args_.insert(args_.end(), args.begin(), args.end());
}

void OpBuffer::clear() noexcept {
    entries_.clear();
    args_.clear();
}

std::vector<std::byte> RunResult::to_bytes() const {
    core::ByteWriter out;
    out.u32(0x31525645U);  // "EVR1"
    out.u8(ok ? 1 : 0);
    out.u32(static_cast<std::uint32_t>(reason));
    out.u32(abort_pc);
    out.u32(abort_detail);
    out.u8(has_value ? 1 : 0);
    out.i64(value);
    out.u32(instructions);
    out.u32(wu);
    out.u32(counters.saturations);
    out.u32(counters.div_zero);
    out.u32(counters.truncated);
    out.u32(counters.draws);
    put_buffer(out, commands);
    put_buffer(out, state_ops);
    return core::to_std_bytes(out.data());
}

std::uint64_t RunResult::hash() const noexcept {
    const std::vector<std::byte> bytes = to_bytes();
    return core::hash_bytes(bytes);
}

RunResult Vm::run(const Program& program, VmHost& host, const RunOptions& options) {
    switch (options.mode) {
        case Mode::Trace:
            return execute<Mode::Trace>(program, host, options, scratch_);
        case Mode::Preview:
            return execute<Mode::Preview>(program, host, options, scratch_);
        case Mode::Run:
            break;
    }
    return execute<Mode::Run>(program, host, options, scratch_);
}

const TraceLeaf* TraceRecorder::first_false() const noexcept {
    for (const TraceLeaf& l : leaves_) {
        if (!l.result) {
            return &l;
        }
    }
    return nullptr;
}

void BreakdownRecorder::step(std::uint16_t code, Word accumulator) {
    steps_.push_back(TraceRecorder::Step{code, accumulator});
}

economy::Breakdown BreakdownRecorder::build(core::Fixed64 total) const {
    economy::Breakdown tree;
    tree.begin(root_code_, economy::Unit::Points, to_points(total.raw()));
    // The tree holds 32 nodes: the root, up to 30 steps and the remainder; later steps fold into it.
    constexpr std::size_t kMaxSteps = economy::kBreakdownCapacity - 2;
    std::int64_t before = 0;
    for (std::size_t i = 0; i < steps_.size() && i < kMaxSteps; ++i) {
        const std::int64_t after = to_points(steps_[i].value);
        (void)tree.add_step(economy::kRootNode, steps_[i].code, before, after);
        before = after;
    }
    (void)tree.add_remainder(economy::kRootNode, remainder_code_);
    return tree;
}

void PreviewRecorder::line(PreviewLineKind kind,
                           std::uint16_t code,
                           Scope target,
                           std::span<const Word> args) {
    PreviewEvent e;
    e.type = PreviewEvent::Type::Line;
    e.line_kind = kind;
    e.code = code;
    e.scope = target;
    e.args.assign(args.begin(), args.end());
    events_.push_back(std::move(e));
}

void PreviewRecorder::group_begin(PreviewGroup group, std::uint32_t index, std::uint32_t value) {
    PreviewEvent e;
    e.type = PreviewEvent::Type::GroupBegin;
    e.group = group;
    e.index = index;
    e.value = value;
    events_.push_back(std::move(e));
}

void PreviewRecorder::group_end() {
    PreviewEvent e;
    e.type = PreviewEvent::Type::GroupEnd;
    events_.push_back(std::move(e));
}

void PreviewRecorder::element(Scope scope) {
    PreviewEvent e;
    e.type = PreviewEvent::Type::Element;
    e.scope = scope;
    events_.push_back(std::move(e));
}

void PreviewRecorder::iter_pick(std::uint32_t candidates) {
    PreviewEvent e;
    e.type = PreviewEvent::Type::IterPick;
    e.index = candidates;
    events_.push_back(std::move(e));
}

}  // namespace pat::sim::events
