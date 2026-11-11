#include "pat/sim/economy/recruits.hpp"

#include "pat/core/assert.hpp"
#include "pat/sim/economy/administrator.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/economy/policy.hpp"
#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <cstddef>

namespace pat::sim::economy {

namespace {

/// Level index clamped into the table, like the taxes and the moods do.
std::int32_t level_permille(const RecruitRules& recruit, std::uint8_t level) noexcept {
    if (recruit.level_count <= 0) {
        return kPermilleOne;
    }
    const auto last = static_cast<std::size_t>(recruit.level_count - 1);
    return recruit.levels[std::min<std::size_t>(level, last)].recruit_permille;
}

/// sum of size x share / 1000 over the recruit groups, each truncated.
std::int64_t base_people(const RecruitRules& recruit,
                         const std::array<std::uint32_t, kGroupCount>& size) noexcept {
    std::int64_t base = 0;
    for (const Group group : kRecruitGroups) {
        const std::size_t g = index_of(group);
        base += mul_permille(static_cast<std::int64_t>(size[g]), recruit.share_permille[g]);
    }
    return base;
}

std::uint32_t to_people(std::int64_t value) noexcept {
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(value, 0, kMaxProvincePersons));
}

}  // namespace

std::uint32_t
compute_recruit_cap(const EconomyRules& rules, const RecruitInput& input, Breakdown* breakdown) {
    const RecruitRules& recruit = rules.recruit;
    const std::int64_t base = base_people(recruit, input.size);
    const bool anarchy = control_state(rules.control, input.control) == ControlState::Anarchy;

    struct Step {
        std::uint16_t code;
        std::int32_t permille;
    };
    const std::array<Step, 2> steps{{
        {term::kRecruitPolicy, level_permille(recruit, input.level)},
        {term::kRecruitControl, control_permille(input.control)},
    }};

    if (breakdown != nullptr) {
        breakdown->begin(term::kRecruitCap, Unit::People, 0);
        (void)breakdown->add_term(kRootNode, term::kRecruitBase, base);
    }
    std::int64_t running = base;
    for (const Step& step : steps) {
        const std::int64_t next = mul_permille(running, step.permille);
        if (breakdown != nullptr && step.permille != kPermilleOne) {
            (void)breakdown->add_step(kRootNode, step.code, running, next, step.permille);
        }
        running = next;
    }
    if (anarchy) {
        if (breakdown != nullptr && running != 0) {
            (void)breakdown->add_step(kRootNode, term::kRecruitAnarchy, running, 0);
        }
        running = 0;
    }
    // the unpaid quota is taken from what is left (hook of E9-T14); never below zero
    const std::int64_t quota = std::min<std::int64_t>(running, input.quota_pending);
    if (breakdown != nullptr && quota != 0) {
        (void)breakdown->add_term(kRootNode, term::kRecruitQuota, -quota);
    }
    const std::uint32_t cap = to_people(running - quota);
    if (breakdown != nullptr) {
        breakdown->set_total(cap);
        PAT_ASSERT(breakdown->is_consistent(), "the recruit breakdown must add up to the potential");
    }
    return cap;
}

RecruitResult compute_recruit_pool(const EconomyRules& rules, const RecruitInput& input) {
    RecruitResult out;
    out.cap = compute_recruit_cap(rules, input);
    const auto pool = static_cast<std::int64_t>(input.pool);
    const auto cap = static_cast<std::int64_t>(out.cap);
    if (pool >= cap) {
        out.new_pool = out.cap;  // a cap that fell cuts the pool at once
    } else {
        const std::int32_t regen = std::clamp(rules.recruit.regen_permille, 0, kPermilleOne);
        std::int64_t step = mul_permille(cap - pool, regen);
        if (step == 0 && regen > 0) {
            step = 1;  // arrives exactly instead of crawling forever
        }
        out.new_pool = static_cast<std::uint32_t>(std::min(cap, pool + step));
    }
    PAT_ASSERT(out.new_pool <= out.cap, "the pool never exceeds the potential after a month");
    return out;
}

std::uint32_t recruit_ceiling(const EconomyRules& rules, const std::array<std::uint32_t, kGroupCount>& size) {
    std::int32_t best = 0;
    for (std::int32_t i = 0; i < rules.recruit.level_count; ++i) {
        best = std::max(best, rules.recruit.levels[static_cast<std::size_t>(i)].recruit_permille);
    }
    if (rules.recruit.level_count <= 0) {
        best = kPermilleOne;
    }
    return to_people(mul_permille(base_people(rules.recruit, size), best));
}

std::uint64_t recruit_population(const std::array<std::uint32_t, kGroupCount>& size) noexcept {
    std::uint64_t total = 0;
    for (const Group group : kRecruitGroups) {
        total += size[index_of(group)];
    }
    return total;
}

std::array<std::uint32_t, kGroupCount> split_conscription(const std::array<std::uint32_t, kGroupCount>& size,
                                                          std::uint32_t count) noexcept {
    std::array<std::uint32_t, kGroupCount> take{};
    const std::uint64_t total = recruit_population(size);
    if (total == 0 || count == 0) {
        return take;
    }
    const std::uint64_t want = std::min<std::uint64_t>(count, total);
    std::uint64_t given = 0;
    for (const Group group : kRecruitGroups) {
        const std::size_t g = index_of(group);
        take[g] = static_cast<std::uint32_t>(want * size[g] / total);  // <= size[g] since want <= total
        given += take[g];
    }
    // the remainder (fewer than one person per group): one at a time, in the order of kRecruitGroups
    while (given < want) {
        for (const Group group : kRecruitGroups) {
            const std::size_t g = index_of(group);
            if (given < want && take[g] < size[g]) {
                ++take[g];
                ++given;
            }
        }
    }
    PAT_ASSERT(given == want, "a conscription takes exactly what it was asked for");
    return take;
}

RecruitInput recruit_input(const World& world, const EconomyRules& rules, ProvinceId province) {
    RecruitInput in;
    if (!world.economy_present() || !world.exists(province)) {
        return in;
    }
    const EconomyState& e = world.economy();
    const std::size_t p = province.index();
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        in.size[g] = e.groups.size[p * kGroupCount + g];
    }
    in.control = e.provinces.control[p];
    in.level = effective_recruit_level(world, rules, province);
    in.pool = e.provinces.recruit_pool[p];
    return in;
}

void apply_province_recruits(World& world, ProvinceId province, const RecruitResult& result) {
    if (!world.economy_present() || !world.exists(province)) {
        return;
    }
    world.economy_columns_mut().recruit_pool[province.index()] = result.new_pool;
}

RecruitResult step_province_recruits(World& world, const EconomyRules& rules, ProvinceId province) {
    if (!world.economy_present() || !world.exists(province)) {
        return {};
    }
    const RecruitResult result = compute_recruit_pool(rules, recruit_input(world, rules, province));
    apply_province_recruits(world, province, result);
    return result;
}

Breakdown explain_recruit_cap(const World& world, const EconomyRules& rules, ProvinceId province) {
    Breakdown out;
    (void)compute_recruit_cap(rules, recruit_input(world, rules, province), &out);
    return out;
}

}  // namespace pat::sim::economy
