#include "pat/sim/economy/unrest.hpp"

#include "pat/core/assert.hpp"
#include "pat/core/rng.hpp"
#include "pat/sim/economy/administrator.hpp"
#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/economy/taxes.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace pat::sim::economy {

using core::Fixed32;

namespace {

constexpr std::int64_t kOne = Fixed32::one_raw;
constexpr std::int64_t kUnrestMaxRaw = 100 * kOne;

Fixed32 from_raw64(std::int64_t raw) noexcept {
    return Fixed32::from_raw(static_cast<std::int32_t>(raw));
}

/// The incident entries of a change of state: UnrestStateChanged (the new state) and, on entering the
/// uprising, Uprising (the unrest in whole points). True when something was written.
bool record_unrest_state(World& world, ProvinceId province, const UnrestResult& result) {
    if (result.before == result.after) {
        return false;
    }
    const DynastyId dynasty = incident_dynasty(world, province);
    bool written =
        world
            .log_incident(
                IncidentKind::UnrestStateChanged, province, dynasty, static_cast<std::int32_t>(result.after))
            .has_value();
    if (result.after == UnrestState::Uprising) {
        written = world
                      .log_incident(IncidentKind::Uprising,
                                    province,
                                    dynasty,
                                    static_cast<std::int32_t>(result.unrest.to_int()))
                      .has_value() ||
                  written;
    }
    return written;
}

}  // namespace

// ---- state -----------------------------------------------------------------------------------------

UnrestState unrest_state(const UnrestRules& rules, Fixed32 unrest) noexcept {
    const std::int64_t raw = unrest.raw();
    if (raw < static_cast<std::int64_t>(rules.calm_below) * kOne) {
        return UnrestState::Calm;
    }
    if (raw < static_cast<std::int64_t>(rules.riot_from) * kOne) {
        return UnrestState::Agitation;
    }
    if (raw < static_cast<std::int64_t>(rules.uprising_from) * kOne) {
        return UnrestState::Riot;
    }
    return UnrestState::Uprising;
}

bool is_uprising(const UnrestRules& rules, Fixed32 unrest) noexcept {
    return unrest_state(rules, unrest) == UnrestState::Uprising;
}

// ---- F9 --------------------------------------------------------------------------------------------

Fixed32 group_unrest(const UnrestRules& rules, Fixed32 mood) noexcept {
    const std::int64_t raw = mul_permille(-static_cast<std::int64_t>(mood.raw()), rules.slope_permille);
    return from_raw64(std::clamp<std::int64_t>(raw, 0, kUnrestMaxRaw));
}

Fixed32 compute_unrest_target(const EconomyRules& rules, const UnrestInput& input, Breakdown* breakdown) {
    const UnrestRules& unrest = rules.unrest;
    // weights and products are unsigned: 6 x 3.2e7 persons x 10^4 x 6.6e6 raw points < 2^64
    std::array<std::uint64_t, kGroupCount> weight{};
    std::uint64_t total_weight = 0;
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        const std::uint64_t persons = std::min<std::uint32_t>(input.size[g], kMaxProvincePersons);
        const auto influence = static_cast<std::uint64_t>(
            std::clamp<std::int32_t>(unrest.influence_permille[g], 0, kMaxMultiplierPermille));
        weight[g] = persons * influence;
        total_weight += weight[g];
    }
    // The average, split by cumulative truncation so that the contributions add up to it exactly.
    std::array<std::int64_t, kGroupCount> contribution{};
    std::uint64_t cumulative_product = 0;
    std::uint64_t previous = 0;
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        cumulative_product +=
            weight[g] * static_cast<std::uint64_t>(group_unrest(unrest, input.mood[g]).raw());
        const std::uint64_t cumulative = total_weight > 0 ? cumulative_product / total_weight : 0;
        contribution[g] = static_cast<std::int64_t>(cumulative - previous);
        previous = cumulative;
    }
    const auto average = static_cast<std::int64_t>(previous);
    const std::int64_t gap = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(rules.control.low_threshold) * kOne - input.control.raw());
    const std::int64_t control = mul_permille(gap, unrest.control_slope_permille);
    const std::int64_t events = input.events.raw();
    const std::int64_t sum = average + control + events;
    const std::int64_t target = std::clamp<std::int64_t>(sum, 0, kUnrestMaxRaw);

    if (breakdown != nullptr) {
        breakdown->begin(term::kUnrestTarget, Unit::Points, target);
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            if (contribution[g] != 0) {
                (void)breakdown->add_term(
                    kRootNode, static_cast<std::uint16_t>(term::kUnrestGroupFirst + g), contribution[g]);
            }
        }
        if (control != 0) {
            (void)breakdown->add_term(
                kRootNode, term::kUnrestControl, control, unrest.control_slope_permille);
        }
        if (events != 0) {
            (void)breakdown->add_term(kRootNode, term::kUnrestEvents, events);
        }
        if (target != sum) {
            (void)breakdown->add_term(kRootNode, term::kUnrestLimit, target - sum);
        }
        PAT_ASSERT(breakdown->is_consistent(), "the unrest breakdown must add up to the target");
    }
    return from_raw64(target);
}

UnrestResult compute_unrest(const EconomyRules& rules, const UnrestInput& input) {
    UnrestResult out;
    out.target = compute_unrest_target(rules, input);
    // a column that was written out of its range is pulled back into it here
    const Fixed32 current = from_raw64(std::clamp<std::int64_t>(input.unrest.raw(), 0, kUnrestMaxRaw));
    out.before = unrest_state(rules.unrest, current);
    out.unrest = approach(current, out.target, rules.unrest.inertia_permille);
    out.after = unrest_state(rules.unrest, out.unrest);
    PAT_ASSERT(out.unrest.raw() >= 0 && out.unrest.raw() <= kUnrestMaxRaw, "unrest must stay within 0..100");
    return out;
}

// ---- F10 -------------------------------------------------------------------------------------------

std::int32_t riot_chance_permille(const UnrestRules& rules, Fixed32 unrest) noexcept {
    const std::int64_t from = static_cast<std::int64_t>(rules.riot_from) * kOne;
    const std::int64_t raw = std::min<std::int64_t>(unrest.raw(), kUnrestMaxRaw);
    if (raw < from) {
        return 0;
    }
    const std::int64_t span = kUnrestMaxRaw - from;
    const std::int64_t range =
        static_cast<std::int64_t>(rules.riot_chance_max_permille) - rules.riot_chance_min_permille;
    const std::int64_t rise = span > 0 ? range * (raw - from) / span : 0;
    return static_cast<std::int32_t>(
        std::clamp<std::int64_t>(rules.riot_chance_min_permille + rise, 0, kPermilleOne));
}

bool roll_riot(core::Pcg32& rng, std::int32_t chance_permille) {
    return static_cast<std::int64_t>(rng.bounded(1000)) < chance_permille;
}

Kopecks riot_loot(const UnrestRules& rules, Kopecks income_last_month) noexcept {
    if (income_last_month <= 0) {
        return 0;
    }
    return std::min(mul_permille(income_last_month, rules.riot_loot_permille), kMaxPostingKop);
}

Fixed32 control_after_riot(const UnrestRules& rules, Fixed32 control) noexcept {
    const std::int64_t raw =
        static_cast<std::int64_t>(control.raw()) - static_cast<std::int64_t>(rules.riot_control_loss) * kOne;
    return from_raw64(std::clamp<std::int64_t>(raw, 0, kUnrestMaxRaw));
}

// ---- province --------------------------------------------------------------------------------------

UnrestInput unrest_input(const World& world, ProvinceId province) {
    UnrestInput in;
    if (!world.economy_present() || !world.exists(province)) {
        return in;
    }
    const EconomyState& e = world.economy();
    const std::size_t p = province.index();
    const std::size_t first = p * kGroupCount;
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        in.size[g] = e.groups.size[first + g];
        in.mood[g] = e.groups.mood[first + g];
    }
    in.control = e.provinces.control[p];
    in.unrest = e.provinces.unrest[p];
    return in;
}

UnrestResult step_province_unrest(World& world, const EconomyRules& rules, ProvinceId province) {
    if (!world.economy_present() || !world.exists(province)) {
        return {};
    }
    const UnrestResult result = compute_unrest(rules, unrest_input(world, province));
    world.economy_columns_mut().unrest[province.index()] = result.unrest;
    (void)record_unrest_state(world, province, result);
    return result;
}

RiotOutcome step_province_riot(World& world, const EconomyRules& rules, ProvinceId province) {
    RiotOutcome out;
    if (!world.economy_present() || !world.exists(province)) {
        return out;
    }
    const std::size_t p = province.index();
    out.control = world.economy().provinces.control[p];
    const Fixed32 unrest = world.economy().provinces.unrest[p];
    if (static_cast<std::int64_t>(unrest.raw()) < static_cast<std::int64_t>(rules.unrest.riot_from) * kOne) {
        return out;
    }
    out.rolled = true;
    out.chance_permille = riot_chance_permille(rules.unrest, unrest);
    // exactly one draw, whatever the chance: the number of draws never depends on an outcome
    out.happened = roll_riot(world.rng().stream("economy.unrest"), out.chance_permille);
    if (!out.happened) {
        return out;
    }
    const Kopecks loot = riot_loot(rules.unrest, world.economy().provinces.income_last_month[p]);
    if (const auto owner = income_recipient(world, province); owner && loot > 0) {
        const Kopecks applied = world.post_money(*owner, LedgerCode::RaidLoss, -loot);
        out.loot_kop = -applied;
        out.refused_kop = std::abs(-loot - applied);
    }
    out.control = control_after_riot(rules.unrest, out.control);
    world.economy_columns_mut().control[p] = out.control;
    const auto param =
        static_cast<std::int32_t>(std::min<Kopecks>(out.loot_kop, std::numeric_limits<std::int32_t>::max()));
    (void)world.log_incident(IncidentKind::Riot, province, incident_dynasty(world, province), param);
    return out;
}

// ---- explanations ----------------------------------------------------------------------------------

Breakdown explain_unrest(const EconomyRules& rules, const UnrestInput& input) {
    Breakdown out;
    (void)compute_unrest_target(rules, input, &out);
    return out;
}

}  // namespace pat::sim::economy
