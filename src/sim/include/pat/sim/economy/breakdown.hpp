#pragma once

#include "pat/sim/economy/money.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// Breakdown: the explanation of a computed number (E9-T04, docs/dev/economy-model.md section 10,
// FR-UI-3). The calculation cores of the economy optionally fill a Breakdown while they compute, so
// a tooltip shows exactly the terms the simulation used and can never disagree with it.
//
// A Breakdown is an ADDITIVE TREE of at most kBreakdownCapacity nodes, stored inline (no allocation).
// Node 0 is the total. Every other node has a parent; the value of a node with children is exactly
// the sum of its children's values. A chain of multipliers is therefore written as a sequence of
// deltas (add_step): each step records `after - before` and keeps the multiplier itself in
// `factor_permille` for a label such as "x1.20". The tree is at most kBreakdownMaxDepth levels deep:
// total -> term -> contribution.
//
// Nodes carry a stable numeric `code` (see pat::sim::economy::term and term_key). Presentation code
// maps a code to a localisation key and the `unit` to a number format; nothing in `sim` knows text.
// Values are int64 in the node's unit. Nothing here uses floating point.

namespace pat::sim::economy {

inline constexpr std::size_t kBreakdownCapacity = 32;
/// Levels in the tree, the total included: total -> term -> contribution.
inline constexpr std::size_t kBreakdownMaxDepth = 3;

/// What the `value` of a node counts. Numeric values are stable.
enum class Unit : std::uint8_t {
    Kopecks = 0,
    People = 1,
    Points = 2,    ///< Fixed32 raw value (mood, control, unrest, wealth)
    Permille = 3,  ///< per-mille (ratios, probabilities)
    Pieces = 4,    ///< units of goods, as Fixed32 raw (Q16.16: value / 65536 = units)
    Ppm = 5,       ///< parts per million (yearly growth rates)
};

using NodeIndex = std::uint8_t;
inline constexpr NodeIndex kRootNode = 0;
/// Returned by the builders when a term could not be added; also the `parent` of the root.
inline constexpr NodeIndex kNoNode = 0xFF;

struct BreakdownNode {
    std::uint16_t code = 0;
    NodeIndex parent = kNoNode;
    Unit unit = Unit::Kopecks;
    std::int64_t value = 0;
    /// The multiplier (per-mille of one) that produced this term, or 0 when it is not a multiplier
    /// step; lets a tooltip show "x1.20" next to the delta.
    std::int32_t factor_permille = 0;

    [[nodiscard]] bool operator==(const BreakdownNode&) const = default;
};

class Breakdown {
public:
    Breakdown() = default;

    /// Starts a new tree whose root (node 0) has the given code, unit and total. Discards anything
    /// built before.
    void begin(std::uint16_t code, Unit unit, std::int64_t total) noexcept;

    /// Adds a term under `parent` and returns its index, or kNoNode when the tree is full, `parent`
    /// is invalid, or the tree would become deeper than kBreakdownMaxDepth. A failure also sets
    /// truncated(): the tree is then incomplete and is_consistent() is not meaningful. The unit is
    /// inherited from the parent.
    NodeIndex add_term(NodeIndex parent,
                       std::uint16_t code,
                       std::int64_t value,
                       std::int32_t factor_permille = 0) noexcept;

    /// Adds the delta of one step of a multiplier chain: value = after - before, factor_permille as
    /// given. Fails like add_term (and on subtraction overflow).
    NodeIndex add_step(NodeIndex parent,
                       std::uint16_t code,
                       std::int64_t before,
                       std::int64_t after,
                       std::int32_t factor_permille = 0) noexcept;

    /// Adds a term that makes the children of `parent` add up to the parent's value ("split by
    /// subtraction": value = parent - sum of the children added so far). Adds nothing and returns
    /// kNoNode when the remainder is 0; sets truncated() if it cannot be represented or added.
    NodeIndex add_remainder(NodeIndex parent, std::uint16_t code) noexcept;

    /// Replaces the total (the root's value).
    void set_total(std::int64_t total) noexcept;
    /// Sets the total to the sum of the root's children; false (total unchanged) on overflow.
    bool set_total_from_children() noexcept;

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }
    [[nodiscard]] const BreakdownNode& node(NodeIndex i) const noexcept { return nodes_[i]; }
    [[nodiscard]] const BreakdownNode& root() const noexcept { return nodes_[kRootNode]; }
    [[nodiscard]] std::int64_t total() const noexcept { return nodes_[kRootNode].value; }

    /// Levels from the root: 0 for the root itself.
    [[nodiscard]] std::size_t depth(NodeIndex i) const noexcept;
    [[nodiscard]] std::size_t child_count(NodeIndex parent) const noexcept;
    /// Sum of the direct children's values; nullopt on overflow.
    [[nodiscard]] std::optional<std::int64_t> children_sum(NodeIndex parent) const noexcept;
    /// First node with the given code, or kNoNode.
    [[nodiscard]] NodeIndex find(std::uint16_t code) const noexcept;

    /// True when the tree is complete (not truncated) and every node that has children equals the
    /// sum of them. Used by tests and by PAT_ASSERT in the calculation cores.
    [[nodiscard]] bool is_consistent() const noexcept;

    [[nodiscard]] bool operator==(const Breakdown& other) const noexcept;

private:
    std::array<BreakdownNode, kBreakdownCapacity> nodes_{};
    std::size_t size_ = 0;
    bool truncated_ = false;
};

// ---- term codes --------------------------------------------------------------------------------

/// Stable codes of breakdown terms (economy-model section 9 and 10). The families are
/// 1000 taxes, 2000 mood, 3000 growth, 4000 output, 5000 consumption and wealth, 6000 control and
/// corruption, 7000 recruits, 8000 loans, 9000 unrest. Codes are never reused or renumbered; new
/// ones are appended to the end of their family. Every code has a localisation key in term_key().
namespace term {

// 1000: tax of a population group (F6)
inline constexpr std::uint16_t kTaxBase = 1001;
inline constexpr std::uint16_t kTaxPolicy = 1002;
inline constexpr std::uint16_t kTaxWealth = 1003;
inline constexpr std::uint16_t kTaxControl = 1004;
inline constexpr std::uint16_t kTaxLaw = 1005;
inline constexpr std::uint16_t kTaxLowControl = 1006;
inline constexpr std::uint16_t kTaxAnarchy = 1007;
inline constexpr std::uint16_t kTaxTotal = 1008;  // the root of a tax breakdown: what the owner keeps
inline constexpr std::uint16_t kTaxLeak = 1010;
inline constexpr std::uint16_t kTaxCrownShare = 1011;
// 2000: mood of a group (F8)
inline constexpr std::uint16_t kMoodBase = 2001;
inline constexpr std::uint16_t kMoodTaxPressure = 2002;
inline constexpr std::uint16_t kMoodRecruitPressure = 2003;
inline constexpr std::uint16_t kMoodCoercion = 2004;
inline constexpr std::uint16_t kMoodReligion = 2005;
inline constexpr std::uint16_t kMoodWealth = 2006;
inline constexpr std::uint16_t kMoodAdministrator = 2007;
inline constexpr std::uint16_t kMoodEvents = 2008;
inline constexpr std::uint16_t kMoodStep = 2009;
// 3000: population growth (F1)
inline constexpr std::uint16_t kGrowthBirth = 3001;
inline constexpr std::uint16_t kGrowthDeath = 3002;
inline constexpr std::uint16_t kGrowthWealth = 3003;
inline constexpr std::uint16_t kGrowthModifier = 3004;
inline constexpr std::uint16_t kGrowthCrowding = 3005;
inline constexpr std::uint16_t kGrowthMigration = 3006;
inline constexpr std::uint16_t kGrowthFamine = 3007;
inline constexpr std::uint16_t kGrowthNet = 3008;  // the root of a growth breakdown: the net yearly rate
// 4000: output and holding income (F2-F4)
inline constexpr std::uint16_t kOutputCapacity = 4001;
inline constexpr std::uint16_t kOutputLabour = 4002;
inline constexpr std::uint16_t kOutputBuildings = 4003;
inline constexpr std::uint16_t kOutputControl = 4004;
inline constexpr std::uint16_t kOutputLosses = 4005;
inline constexpr std::uint16_t kOutputHarvest = 4006;
inline constexpr std::uint16_t kOutputLabourWorkforce = 4007;  // labour factor: workers / base, clamped
inline constexpr std::uint16_t kOutputLabourMood = 4008;       // labour factor: the sullen peasants
inline constexpr std::uint16_t kOutputTotal = 4009;            // the root of an output breakdown
inline constexpr std::uint16_t kOutputValue = 4010;
inline constexpr std::uint16_t kOutputOwnerShare = 4011;
inline constexpr std::uint16_t kOutputIncome = 4012;  // the root of a holding income breakdown
// 5000: consumption, stock and wealth (F5)
inline constexpr std::uint16_t kConsumptionNeed = 5001;
inline constexpr std::uint16_t kConsumptionStock = 5002;
inline constexpr std::uint16_t kConsumptionShortage = 5003;
inline constexpr std::uint16_t kConsumptionOpening = 5004;   // grain stock at the start of the month
inline constexpr std::uint16_t kConsumptionOutput = 5005;    // grain produced this month
inline constexpr std::uint16_t kConsumptionStockCap = 5006;  // surplus above the stock cap, lost
inline constexpr std::uint16_t kWealthBase = 5010;
inline constexpr std::uint16_t kWealthShortage = 5011;
inline constexpr std::uint16_t kWealthBuildings = 5014;
inline constexpr std::uint16_t kWealthTarget = 5015;  // the root of a wealth breakdown: the target
// 6000: control and corruption (F7)
inline constexpr std::uint16_t kControlAdministrator = 6001;
inline constexpr std::uint16_t kControlBuildings = 6002;
inline constexpr std::uint16_t kControlUnrest = 6003;
inline constexpr std::uint16_t kControlBase = 6004;
inline constexpr std::uint16_t kControlTarget = 6005;  // the root of a control breakdown: the target
inline constexpr std::uint16_t kControlLimit = 6006;   // the clamp to 0..100
inline constexpr std::uint16_t kCorruptionBase = 6101;
inline constexpr std::uint16_t kCorruptionIntegrity = 6102;
inline constexpr std::uint16_t kCorruptionOversight = 6103;
inline constexpr std::uint16_t kCorruptionLowControl = 6104;
inline constexpr std::uint16_t kCorruptionTarget = 6105;  // the root of a corruption breakdown: the target
inline constexpr std::uint16_t kCorruptionLimit = 6106;   // the clamp to 0..1
// 7000: recruits (F11)
inline constexpr std::uint16_t kRecruitBase = 7001;
inline constexpr std::uint16_t kRecruitPolicy = 7002;
inline constexpr std::uint16_t kRecruitControl = 7003;
inline constexpr std::uint16_t kRecruitQuota = 7004;
inline constexpr std::uint16_t kRecruitRegen = 7005;
// 8000: loan limit (F13)
inline constexpr std::uint16_t kLoanBase = 8001;
inline constexpr std::uint16_t kLoanHonor = 8002;
inline constexpr std::uint16_t kLoanDebt = 8003;
// 9000: unrest (F9). The contribution of group g is kUnrestGroupFirst + g.
inline constexpr std::uint16_t kUnrestGroupFirst = 9010;
inline constexpr std::uint16_t kUnrestControl = 9020;
inline constexpr std::uint16_t kUnrestEvents = 9021;

}  // namespace term

/// Localisation key of a term code ("economy.tax.base"), or an empty view for an unknown code.
/// The returned view refers to static storage.
[[nodiscard]] std::string_view term_key(std::uint16_t code) noexcept;

/// Number of issued term codes (every one has a key).
[[nodiscard]] std::size_t term_code_count() noexcept;
/// The i-th issued term code in ascending order (i < term_code_count()).
[[nodiscard]] std::uint16_t term_code_at(std::size_t i) noexcept;

}  // namespace pat::sim::economy
