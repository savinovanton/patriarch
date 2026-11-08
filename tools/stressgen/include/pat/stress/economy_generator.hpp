#pragma once

#include "pat/core/result.hpp"
#include "pat/sim/world.hpp"

#include <cstddef>
#include <cstdint>

namespace pat::stress {

/// Synthetic economy for a stress world (E9-T05, docs/dev/economy-model.md section 14). Like the rest of
/// stressgen it is benchmark input, not balance or content: the numbers are plausible and inside the
/// bounds of the invariants, nothing more.
struct EconomyConfig {
    /// Fill the economy of the generated world. Off by default: the old profiles stay byte for byte.
    bool enabled = false;
    /// Closed months of the statement to fill, 0..12 (12 = the full ring of the real game). The open
    /// month is always the month of the snapshot date, so the closed ones lie in the year before it.
    std::uint32_t closed_months = 12;
    /// Provinces with a construction queue (2-3 entries each), in permille of the provinces.
    std::uint32_t building_sites_permille = 200;
};

/// Hash of the rules written into the synthetic worlds. `EconomyRules` has no hash yet (it comes with
/// the content layer, economy-model section 13.3), so the generator stamps a constant.
inline constexpr std::uint64_t kStressRulesHash = 0x5354524553533031ULL;  // "STRESS01"

/// Switches the economy of `world` on and fills it: the population by the six groups, kinds, owners,
/// policies, administrators, buildings, construction sites, a statement of up to 12 closed months for
/// the crown and every dynasty (taxes, upkeep and transfers), loans, quotas and their outcomes, a full
/// ring of incidents. Deterministic: the draws come from named streams of `world.rng()` (created only
/// here, so a world without economy keeps its random state), nothing else is read.
///
/// The ledger, the incident log and the people go through the public API of World (post_money,
/// transfer, close_ledger_month, log_incident, set_group_size, ...). The loans, quotas, outcomes, the
/// construction queue, the counters and the month the statement starts in have no writer in World yet
/// (E9-T14..T16 add them): the tables are filled directly, in the way the tests of the economy do, and
/// check_invariants() decides whether the result is valid.
///
/// Errors: the economy is on already, the counts are impossible, or an invariant is violated.
[[nodiscard]] core::Result<void> populate_economy(sim::World& world, const EconomyConfig& config);

/// Bytes held by the tables of the economy, by table (economy-model section 4.11). `*_used` counts the
/// elements (size x sizeof), `*_allocated` what the vectors reserved (capacity x sizeof). The account
/// table is private: it is sized exactly (rows x lines), so used == allocated there.
struct EconomyMemory {
    std::size_t provinces = 0;  ///< province count the numbers belong to
    std::size_t dynasties = 0;

    std::size_t groups_used = 0;
    std::size_t province_used = 0;
    std::size_t dynasty_used = 0;
    std::size_t construction_used = 0;
    std::size_t loans_used = 0;
    std::size_t quotas_used = 0;
    std::size_t outcomes_used = 0;
    std::size_t incidents_used = 0;
    std::size_t ledger_bytes = 0;  ///< the account table: balances and the statement with its ring

    std::size_t groups_allocated = 0;
    std::size_t province_allocated = 0;
    std::size_t dynasty_allocated = 0;
    std::size_t construction_allocated = 0;
    std::size_t loans_allocated = 0;
    std::size_t quotas_allocated = 0;
    std::size_t outcomes_allocated = 0;
    std::size_t incidents_allocated = 0;

    [[nodiscard]] std::size_t used_without_ledger() const noexcept {
        return groups_used + province_used + dynasty_used + construction_used + loans_used + quotas_used +
               outcomes_used + incidents_used;
    }
    [[nodiscard]] std::size_t allocated_without_ledger() const noexcept {
        return groups_allocated + province_allocated + dynasty_allocated + construction_allocated +
               loans_allocated + quotas_allocated + outcomes_allocated + incidents_allocated;
    }
    [[nodiscard]] std::size_t used_total() const noexcept { return used_without_ledger() + ledger_bytes; }
    [[nodiscard]] std::size_t allocated_total() const noexcept {
        return allocated_without_ledger() + ledger_bytes;
    }
};

/// Sums the sizes of the economy tables of `world` (all zero without economy). This is the counter
/// economy-model section 14 calls `economy_memory_bytes()`; it lives here, in the tool, because
/// nothing in the game needs it.
[[nodiscard]] EconomyMemory economy_memory(const sim::World& world);

}  // namespace pat::stress
