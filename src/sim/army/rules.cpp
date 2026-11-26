#include "pat/sim/army/rules.hpp"

#include "pat/core/hash_stream.hpp"
#include "pat/sim/rules_check.hpp"

#include <algorithm>
#include <format>

namespace pat::sim::army {
namespace {
using Check = core::Result<void, ArmyRulesError>;
Check fail(std::string field, std::string message) {
    return rules_check::fail<ArmyRulesError>(std::move(message), std::move(field));
}
Check range(std::string_view field, std::int64_t value, std::int64_t lo, std::int64_t hi) {
    return rules_check::in_range<ArmyRulesError>(field, value, lo, hi);
}
}  // namespace
ArmyRules ArmyRules::defaults() {
    ArmyRules r;
    r.branches = {{{1000, 20, 40, 300, 1000, 1000, 300, 90},
                   {1500, 30, 90, 900, 1100, 1500, 50, 120},
                   {2500, 12, 70, 1500, 1000, 1000, 4000, 150},
                   {600, 28, 15, 60, 1200, 800, 150, 30}}};
    r.terrains = {{{1000, 1000, 1000, CounterClass::Open},
                   {1100, 1000, 700, CounterClass::Open},
                   {700, 1150, 700, CounterClass::Rough},
                   {500, 1250, 500, CounterClass::Rough},
                   {800, 1200, 800, CounterClass::Rough},
                   {500, 1350, 500, CounterClass::Rough}}};
    r.seasons = {{{1100, 800},
                  {1100, 750},
                  {800, 700},
                  {550, 700},
                  {700, 750},
                  {1000, 900},
                  {1000, 1100},
                  {1000, 1200},
                  {900, 1200},
                  {650, 1000},
                  {750, 900},
                  {1050, 850}}};
    r.counter[0][0] = {1000, 900, 900, 1100};
    r.counter[0][1] = {1250, 1000, 1300, 1300};
    r.counter[0][2] = {1150, 1200, 1000, 1250};
    r.counter[0][3] = {850, 850, 900, 1000};
    r.counter[1][0] = {1000, 1200, 1000, 1100};
    r.counter[1][1] = {850, 1000, 1100, 900};
    r.counter[1][2] = {1000, 1000, 1000, 1000};
    r.counter[1][3] = {1100, 1000, 1100, 1000};
    r.sigmoid = {500, 683, 823, 910, 956, 979, 990, 995, 998, 999, 1000};
    r.siege_defence = {200, 500, 900, 1500, 2400, 3600};
    r.fortress_upkeep = {0, 6000, 15000, 30000, 60000, 100000};
    r.front_enemy_base = {1000, 3000, 6000, 500};
    r.arrears = {{{0, 0}, {-1000, 0}, {-1500, 30}, {-2000, 80}}};
    return r;
}

std::uint64_t ArmyRules::hash() const noexcept {
    core::HashStream h;
    h.u32(1);  // Typed army balance layout version; independent of save/state schemas.
#define PAT_ARMY_HASH(group, name, def, lo, hi) h.i32(group##_##name);
    PAT_ARMY_BALANCE_FIELDS(PAT_ARMY_HASH)
#undef PAT_ARMY_HASH
    for (const auto& b : branches) {
        for (const auto v :
             {b.cv, b.speed_kmd, b.upkeep_kop, b.equip_kop, b.vuln, b.need, b.siege_value, b.train_days})
            h.i32(v);
    }
    for (const auto& t : terrains) {
        h.i32(t.move_pm);
        h.i32(t.defence_pm);
        h.i32(t.supply_pm);
        h.u8(static_cast<std::uint8_t>(t.counter_class));
    }
    for (const auto& s : seasons) {
        h.i32(s.move_pm);
        h.i32(s.supply_pm);
    }
    for (const auto& c : counter)
        for (const auto& row : c)
            for (const auto v : row)
                h.i32(v);
    for (const auto v : sigmoid)
        h.i32(v);
    for (const auto v : siege_defence)
        h.i32(v);
    for (const auto v : fortress_upkeep)
        h.i32(v);
    for (const auto v : front_enemy_base)
        h.i32(v);
    for (const auto& a : arrears) {
        h.i32(a.morale);
        h.i32(a.desert);
    }
    return h.value();
}

Check validate(const ArmyRules& r) {
#define PAT_ARMY_CHECK(group, name, def, lo, hi)                                                             \
    PAT_RULES_CHECK(range(#group "." #name, r.group##_##name, lo, hi));
    PAT_ARMY_BALANCE_FIELDS(PAT_ARMY_CHECK)
#undef PAT_ARMY_CHECK
    for (std::size_t i = 0; i < r.branches.size(); ++i) {
        const auto& b = r.branches[i];
        const auto prefix = std::format("branch.{}.", kBranchNames[i]);
        PAT_RULES_CHECK(range(prefix + "cv", b.cv, 1, 10000));
        PAT_RULES_CHECK(range(prefix + "speed_kmd", b.speed_kmd, 1, 1000));
        PAT_RULES_CHECK(range(prefix + "upkeep_kop", b.upkeep_kop, 0, 1000000));
        PAT_RULES_CHECK(range(prefix + "equip_kop", b.equip_kop, 0, 1000000));
        PAT_RULES_CHECK(range(prefix + "vuln", b.vuln, 1, 10000));
        PAT_RULES_CHECK(range(prefix + "need", b.need, 1, 10000));
        PAT_RULES_CHECK(range(prefix + "siege_value", b.siege_value, 0, 10000));
        PAT_RULES_CHECK(range(prefix + "train_days", b.train_days, 1, 3650));
    }
    for (std::size_t i = 0; i < r.terrains.size(); ++i) {
        const auto& t = r.terrains[i];
        const auto p = std::format("terrain.{}.", kTerrainNames[i]);
        PAT_RULES_CHECK(range(p + "move_pm", t.move_pm, 1, 4000));
        PAT_RULES_CHECK(range(p + "defence_pm", t.defence_pm, 1, 4000));
        PAT_RULES_CHECK(range(p + "supply_pm", t.supply_pm, 1, 4000));
        PAT_RULES_CHECK(range(p + "class", static_cast<std::uint8_t>(t.counter_class), 0, 1));
    }
    for (std::size_t i = 0; i < r.seasons.size(); ++i) {
        PAT_RULES_CHECK(range(std::format("season.{}.move_pm", i + 1), r.seasons[i].move_pm, 1, 4000));
        PAT_RULES_CHECK(range(std::format("season.{}.supply_pm", i + 1), r.seasons[i].supply_pm, 1, 4000));
    }
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < 4; ++i)
            for (std::size_t j = 0; j < 4; ++j)
                PAT_RULES_CHECK(range(
                    std::format(
                        "counter.{}.{}.{}", c == 0 ? "open" : "rough", kBranchNames[i], kBranchNames[j]),
                    r.counter[c][i][j],
                    1,
                    4000));
    for (std::size_t i = 0; i < r.sigmoid.size(); ++i) {
        const auto p = std::format("battle.sigmoid[{}]", i);
        PAT_RULES_CHECK(range(p, r.sigmoid[i], 500, 1000));
        if (i > 0 && r.sigmoid[i] < r.sigmoid[i - 1])
            return fail(p, "sigmoid must not decrease");
    }
    if (r.sigmoid.front() != 500 || r.sigmoid.back() != 1000)
        return fail("battle.sigmoid", "sigmoid endpoints must be 500 and 1000");
    for (std::size_t i = 0; i < 6; ++i) {
        PAT_RULES_CHECK(range(std::format("siege.defence[{}]", i), r.siege_defence[i], 1, 1000000));
        PAT_RULES_CHECK(range(std::format("upkeep.fortress[{}]", i), r.fortress_upkeep[i], 0, 100000000));
        if (i > 0 && r.siege_defence[i] <= r.siege_defence[i - 1])
            return fail("siege.defence", "defence must increase with level");
        if (i > 0 && r.fortress_upkeep[i] < r.fortress_upkeep[i - 1])
            return fail("upkeep.fortress", "fortress upkeep must not decrease");
    }
    for (std::size_t i = 0; i < 4; ++i) {
        PAT_RULES_CHECK(range(std::format("front.enemy_base[{}]", i), r.front_enemy_base[i], 0, 10000));
        PAT_RULES_CHECK(range(std::format("upkeep.arrears[{}].morale", i), r.arrears[i].morale, -10000, 0));
        PAT_RULES_CHECK(range(std::format("upkeep.arrears[{}].desert", i), r.arrears[i].desert, 0, 1000));
        if (i > 0 &&
            (r.arrears[i].morale > r.arrears[i - 1].morale || r.arrears[i].desert < r.arrears[i - 1].desert))
            return fail("upkeep.arrears", "arrears penalties must not improve");
    }
    if (r.limits_min_size > r.limits_max_size || r.limits_min_rebels > r.limits_max_size ||
        r.levy_min_size > r.limits_max_size)
        return fail("limits.max_size", "max_size must cover all minimum army sizes");
    if (std::max({r.limits_dynasty, r.limits_crown, r.limits_rebels}) > r.limits_world)
        return fail("limits.world", "owner limits exceed the world limit");
    if (r.battle_p_min + r.battle_p_max != 1000)
        return fail("battle.p_max", "probability clamps must be symmetric");
    if (r.front_low + r.front_hysteresis >= r.front_high - r.front_hysteresis)
        return fail("front.hysteresis", "front hysteresis bands overlap");
    if (r.rebels_share_min > r.rebels_share_max)
        return fail("rebels.share_max", "rebel population shares are reversed");
    if (r.rebels_infantry_pm + r.rebels_cavalry_max_pm + 999 / r.rebels_cavalry_noise_divisor > 1000)
        return fail("rebels.infantry_pm", "rebel branch shares exceed 1000");
    if (r.rebels_quality_base + r.rebels_quality_range - 1 > 100)
        return fail("rebels.quality_range", "rebel quality exceeds 100");
    if (r.battle_kill_loser_pm + r.battle_wound_loser_pm > 1000 ||
        r.battle_kill_winner_pm + r.battle_wound_winner_pm > 1000)
        return fail("battle.kill_loser_pm", "commander fate probabilities exceed 1000");
    if (r.levy_cooldown_days < r.levy_days)
        return fail("levy.cooldown_days", "levy cooldown must cover its lifetime");
    return Check::ok();
}
}  // namespace pat::sim::army
