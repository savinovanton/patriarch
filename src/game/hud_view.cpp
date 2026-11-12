#include "pat/game/hud_view.hpp"

#include "pat/core/assert.hpp"
#include "pat/game/world_view.hpp"
#include "pat/sim/economy/modifiers.hpp"
#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/tables.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace pat::game {
namespace {

using sim::economy::AccountRef;
using sim::economy::AccountTable;
using sim::economy::Kopecks;

[[nodiscard]] bool ends_with(const std::string& text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// The block of the top bar with `key`; the placeholder snapshot always has treasury and militia.
[[nodiscard]] ui::HudResource& resource_of(ui::HudSnapshot& hud, std::string_view key) {
    const auto it = std::ranges::find(hud.resources, key, &ui::HudResource::key);
    PAT_ASSERT(it != hud.resources.end(), "the placeholder HUD lacks a resource");
    return *it;
}

/// Change of the granaries of the player's provinces over one month at the current rates, in raw Fixed32.
[[nodiscard]] std::int64_t monthly_grain_change_raw(const sim::World& world,
                                                    sim::DynastyId player,
                                                    const sim::economy::EconomyRules& rules) {
    const sim::economy::StaticPriceSource prices(rules.production);
    std::int64_t change = 0;
    for (const sim::ProvinceId province : world.provinces_of(player)) {
        const sim::economy::ProvinceModifiers modifiers =
            sim::economy::province_modifiers(world, rules, nullptr, province);
        const sim::economy::ProvinceProduction month = sim::economy::compute_province_production(
            rules, sim::economy::production_input(world, prices, province, &modifiers));
        change += static_cast<std::int64_t>(month.new_grain_stock.raw()) -
                  static_cast<std::int64_t>(world.economy().provinces.grain_stock[province.index()].raw());
    }
    return change;
}

}  // namespace

std::int64_t kopecks_to_roubles(Kopecks amount) noexcept {
    return amount / sim::economy::kKopecksPerRouble;
}

std::string clan_genitive(const std::string& clan) {
    // A trailing number of a repeated name ("Стрешневы 2") stays as it is.
    std::string head = clan;
    std::string tail;
    const std::size_t space = clan.rfind(' ');
    if (space != std::string::npos) {
        head = clan.substr(0, space);
        tail = clan.substr(space);
    }
    if (ends_with(head, "ие")) {
        head.replace(head.size() - 4, 4, "их");
    } else if (ends_with(head, "ы")) {
        head.replace(head.size() - 2, 2, "ых");
    }
    return head + tail;
}

sim::DynastyId first_owner(const sim::World& world) {
    const sim::ProvinceTable& table = world.provinces();
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table.owner[i].valid()) {
            return table.owner[i];
        }
    }
    return {};
}

ui::HudSnapshot make_hud(const sim::World& world, sim::DynastyId player, const HudOptions& options) {
    ui::HudSnapshot hud = ui::make_test_hud();
    if (!world.economy_present() || !player.valid() || !world.exists(player)) {
        return hud;
    }
    const auto row = world.resolve_account(AccountRef::of(player));
    if (!row) {
        return hud;
    }
    const sim::economy::EconomyState& economy = world.economy();
    hud.house = clan_genitive(dynasty_name(player));

    // Treasury: the balance and the net of the last closed month (the sum of its statement lines).
    ui::HudResource& treasury = resource_of(hud, "treasury");
    treasury.value = kopecks_to_roubles(economy.accounts.treasury(*row));
    treasury.delta = economy.meta.closed_months > 0
                         ? kopecks_to_roubles(AccountTable::sum(economy.accounts.ring_lines(*row, 0)))
                         : 0;
    treasury.note = ui::HudNote::Delta;

    // Provinces of the house: holdings, granaries and the recruit pools.
    std::int64_t grain_raw = 0;
    std::int64_t pools = 0;
    hud.holdings.clear();
    for (const sim::ProvinceId province : world.provinces_of(player)) {
        const std::size_t p = province.index();
        ui::HudHolding holding;
        holding.id = static_cast<std::uint32_t>(p);
        if (options.province_name) {
            holding.name = options.province_name(province);
        }
        holding.population = world.provinces().population[p].to_int();
        hud.holdings.push_back(std::move(holding));
        grain_raw += economy.provinces.grain_stock[p].raw();
        pools += economy.provinces.recruit_pool[p];
    }
    hud.grain.stock = grain_raw / sim::economy::kQuantityOne;
    hud.grain.delta = options.rules != nullptr ? monthly_grain_change_raw(world, player, *options.rules) /
                                                     sim::economy::kQuantityOne
                                               : 0;

    // "Army" until E16: the reserve of recruits, out of what a full conscription would give.
    ui::HudResource& militia = resource_of(hud, "militia");
    const std::int64_t reserve = economy.dynasties.manpower_reserve[player.index()];
    militia.value = reserve;
    militia.capacity = reserve + pools;
    militia.note = ui::HudNote::Capacity;
    return hud;
}

}  // namespace pat::game
