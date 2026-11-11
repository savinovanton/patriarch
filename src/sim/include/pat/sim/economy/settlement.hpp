#pragma once

#include "pat/sim/date.hpp"
#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/entity_ids.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Closing of the month, flat expenses and affordability (E9-T09, docs/dev/economy-model.md section
// 6.3 and 7.1, GDD 03 section 7.3).
//
// The system `economy.settlement` runs on the 1st of every month, BEFORE the slot of the first day of
// `economy.provinces` (registration order, section 7.1), and does, for the month that ends:
//   1. posts the flat expenses of the closing month to every account, ascending by row: the upkeep of
//      the buildings of its provinces (`building_upkeep`), then the lines of the registered
//      ExpenseSources in ascending order of the ledger code;
//   2. (later tasks: quotas and loans that fall due - E9-T14, E9-T15; interest - E9-T15);
//   3. rotates the ring of the statement (World::close_ledger_month): the open lines become the newest
//      closed month, the balance becomes the opening balance, the open lines restart at zero.
// Every posting goes through World::post_money; nothing here writes a balance. An expense is NEVER
// refused for lack of money: the balance may go negative (that is what debt is); only the commands that
// spend (construction, repayment, ...) check `can_afford` before they act.
//
// Everything is integer-only, has no random numbers and keeps no state of its own: the statement lives
// in the World, the rules and the sources are content.

namespace pat::sim {
class World;
}

namespace pat::sim::economy {

// ---- expense sources (the interface for E7, E10, E16) --------------------------------------------

/// One flat expense of an account for the month: the ledger line and how much the account pays
/// (positive kopecks; the settlement posts it as an expense).
struct ExpenseLine {
    LedgerCode code = LedgerCode::CourtUpkeep;
    Kopecks amount_kop = 0;
};

/// The ledger codes an ExpenseSource may use: the flat-expense lines that belong to other epics
/// (`court_upkeep`, `salaries_out`, `army_upkeep`, `gifts_bribes`, `church_donations`, `dowry_out`).
/// The other lines have their own writers (upkeep of buildings, construction, embezzlement, raids,
/// interest, quotas) or are income or transfers.
[[nodiscard]] bool is_expense_source_code(LedgerCode code) noexcept;

/// A source of flat expenses of another epic (court and family - E7/E10, armies and fortresses - E16).
/// Stateless from the point of view of the economy: it reads the World and answers; it must not change
/// anything and must be deterministic. In M1 no source is registered (the court and the army cost 0).
class ExpenseSource {
public:
    virtual ~ExpenseSource() = default;

    /// Appends the expenses of `account` for the month that starts on `month_start` (the month being
    /// closed by the settlement; the next month when a forecast asks). Lines with a code outside
    /// `is_expense_source_code` or a non-positive amount are ignored. In the "one treasury" mode of Q7
    /// the holder dynasty is asked twice: as itself and as the crown. During the settlement the accounts
    /// before `account` have already been charged: do not read balances to decide the amounts.
    virtual void collect(const World& world,
                         AccountRef account,
                         Date month_start,
                         std::vector<ExpenseLine>& out) const = 0;
};

/// The registered sources, asked in this order (their lines are then sorted by code, stably).
using ExpenseSources = std::vector<std::shared_ptr<const ExpenseSource>>;

// ---- upkeep of the buildings ---------------------------------------------------------------------

/// Upkeep per month by building id (the stable id written into a province slot); 0 for "no building"
/// and for an id the rules do not know.
using UpkeepTable = std::array<Kopecks, 256>;

[[nodiscard]] UpkeepTable make_upkeep_table(const BuildingRules& rules) noexcept;

/// Upkeep per month of the buildings that stand in the slots of a province (0 without economy).
[[nodiscard]] Kopecks
province_building_upkeep(const World& world, const UpkeepTable& table, ProvinceId province) noexcept;

/// What every account pays for the buildings of its provinces, per month, positive kopecks, indexed by
/// the row of the account (`World::resolve_account`): the payer is the one who gets the income of the
/// province (a dynasty - its account, the crown - the crown account; an unowned province and one held
/// by the Church or free people pay nobody in M1). Control does not matter: a building in a province in
/// turmoil still costs. Empty without economy.
[[nodiscard]] std::vector<Kopecks> building_upkeep_by_account(const World& world, const BuildingRules& rules);

// ---- the month ------------------------------------------------------------------------------------

/// What the settlement did.
struct SettlementReport {
    bool closed = false;          ///< a month was closed
    Date closed_month_start;      ///< first day of the month that was closed (when `closed`)
    Kopecks building_upkeep = 0;  ///< what was really posted for buildings, all accounts, positive
    Kopecks other_expenses = 0;   ///< what was really posted from the ExpenseSources, positive
    /// What the accounts refused of the expenses (requested minus posted, positive; the balance of an
    /// account saturates at -kMaxTreasuryKop): 0 unless a saturation happened, so it never passes unseen.
    Kopecks refused_kop = 0;
};

/// Closes the open month of the statement on the 1st day `month_start` of the next one: posts the
/// flat expenses of the closing month (they belong to it), rotates the ring. Does nothing (returns
/// `closed == false`) in a world without economy, when `month_start` is not the first day of a month,
/// or when the open month is not earlier than it (the month is already closed). Missed months are
/// skipped without back-charging: one month of expenses is posted per call.
SettlementReport
settle_month(World& world, const EconomyRules& rules, const ExpenseSources& sources, Date month_start);

// ---- affordability --------------------------------------------------------------------------------

/// The rule of the commands that spend money: the account must hold at least `cost_kop` (a balance in
/// debt affords nothing that costs; a cost of zero or less - a free command, a refund - is always
/// affordable). False for an account that does not exist and in a world without economy.
[[nodiscard]] bool can_afford(const World& world, AccountRef account, Kopecks cost_kop) noexcept;

}  // namespace pat::sim::economy
