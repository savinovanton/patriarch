#include "pat/sim/economy/breakdown.hpp"

#include <algorithm>

namespace pat::sim::economy {

namespace {

struct TermKey {
    std::uint16_t code;
    std::string_view key;
};

// One row per issued term code, ascending by code (checked by a test). Keys are localisation keys.
constexpr std::array kTermKeys{
    TermKey{term::kTaxBase, "economy.tax.base"},
    TermKey{term::kTaxPolicy, "economy.tax.policy"},
    TermKey{term::kTaxWealth, "economy.tax.wealth"},
    TermKey{term::kTaxControl, "economy.tax.control"},
    TermKey{term::kTaxLaw, "economy.tax.law"},
    TermKey{term::kTaxLowControl, "economy.tax.low_control"},
    TermKey{term::kTaxAnarchy, "economy.tax.anarchy"},
    TermKey{term::kTaxLeak, "economy.tax.leak"},
    TermKey{term::kTaxCrownShare, "economy.tax.crown_share"},

    TermKey{term::kMoodBase, "economy.mood.base"},
    TermKey{term::kMoodTaxPressure, "economy.mood.tax_pressure"},
    TermKey{term::kMoodRecruitPressure, "economy.mood.recruit_pressure"},
    TermKey{term::kMoodCoercion, "economy.mood.coercion"},
    TermKey{term::kMoodReligion, "economy.mood.religion"},
    TermKey{term::kMoodWealth, "economy.mood.wealth"},
    TermKey{term::kMoodAdministrator, "economy.mood.administrator"},
    TermKey{term::kMoodEvents, "economy.mood.events"},
    TermKey{term::kMoodStep, "economy.mood.step"},

    TermKey{term::kGrowthBirth, "economy.growth.birth"},
    TermKey{term::kGrowthDeath, "economy.growth.death"},
    TermKey{term::kGrowthWealth, "economy.growth.wealth"},
    TermKey{term::kGrowthModifier, "economy.growth.modifier"},
    TermKey{term::kGrowthCrowding, "economy.growth.crowding"},
    TermKey{term::kGrowthMigration, "economy.growth.migration"},
    TermKey{term::kGrowthFamine, "economy.growth.famine"},
    TermKey{term::kGrowthNet, "economy.growth.net"},

    TermKey{term::kOutputCapacity, "economy.output.capacity"},
    TermKey{term::kOutputLabour, "economy.output.labour"},
    TermKey{term::kOutputBuildings, "economy.output.buildings"},
    TermKey{term::kOutputControl, "economy.output.control"},
    TermKey{term::kOutputLosses, "economy.output.losses"},
    TermKey{term::kOutputHarvest, "economy.output.harvest"},
    TermKey{term::kOutputValue, "economy.output.value"},
    TermKey{term::kOutputOwnerShare, "economy.output.owner_share"},

    TermKey{term::kConsumptionNeed, "economy.consumption.need"},
    TermKey{term::kConsumptionStock, "economy.consumption.stock"},
    TermKey{term::kConsumptionShortage, "economy.consumption.shortage"},
    TermKey{term::kWealthBase, "economy.wealth.base"},
    TermKey{term::kWealthShortage, "economy.wealth.shortage"},
    TermKey{term::kWealthBuildings, "economy.wealth.buildings"},

    TermKey{term::kControlAdministrator, "economy.control.administrator"},
    TermKey{term::kControlBuildings, "economy.control.buildings"},
    TermKey{term::kControlUnrest, "economy.control.unrest"},
    TermKey{term::kControlBase, "economy.control.base"},
    TermKey{term::kCorruptionBase, "economy.corruption.base"},
    TermKey{term::kCorruptionIntegrity, "economy.corruption.integrity"},
    TermKey{term::kCorruptionOversight, "economy.corruption.oversight"},
    TermKey{term::kCorruptionLowControl, "economy.corruption.low_control"},

    TermKey{term::kRecruitBase, "economy.recruit.base"},
    TermKey{term::kRecruitPolicy, "economy.recruit.policy"},
    TermKey{term::kRecruitControl, "economy.recruit.control"},
    TermKey{term::kRecruitQuota, "economy.recruit.quota"},
    TermKey{term::kRecruitRegen, "economy.recruit.regen"},

    TermKey{term::kLoanBase, "economy.loan.base"},
    TermKey{term::kLoanHonor, "economy.loan.honor"},
    TermKey{term::kLoanDebt, "economy.loan.debt"},

    TermKey{term::kUnrestGroupFirst + 0, "economy.unrest.nobility"},
    TermKey{term::kUnrestGroupFirst + 1, "economy.unrest.clergy"},
    TermKey{term::kUnrestGroupFirst + 2, "economy.unrest.townsfolk"},
    TermKey{term::kUnrestGroupFirst + 3, "economy.unrest.peasants"},
    TermKey{term::kUnrestGroupFirst + 4, "economy.unrest.cossacks"},
    TermKey{term::kUnrestGroupFirst + 5, "economy.unrest.natives"},
    TermKey{term::kUnrestControl, "economy.unrest.control"},
    TermKey{term::kUnrestEvents, "economy.unrest.events"},
};

}  // namespace

// ---- Breakdown ---------------------------------------------------------------------------------

void Breakdown::begin(std::uint16_t code, Unit unit, std::int64_t total) noexcept {
    nodes_ = {};
    nodes_[kRootNode] = BreakdownNode{code, kNoNode, unit, total, 0};
    size_ = 1;
    truncated_ = false;
}

NodeIndex Breakdown::add_term(NodeIndex parent,
                              std::uint16_t code,
                              std::int64_t value,
                              std::int32_t factor_permille) noexcept {
    if (size_ == 0 || parent >= size_ || size_ >= kBreakdownCapacity ||
        depth(parent) + 1 >= kBreakdownMaxDepth) {
        truncated_ = true;
        return kNoNode;
    }
    const auto index = static_cast<NodeIndex>(size_);
    nodes_[index] = BreakdownNode{code, parent, nodes_[parent].unit, value, factor_permille};
    ++size_;
    return index;
}

NodeIndex Breakdown::add_step(NodeIndex parent,
                              std::uint16_t code,
                              std::int64_t before,
                              std::int64_t after,
                              std::int32_t factor_permille) noexcept {
    const auto delta = checked_sub(after, before);
    if (!delta) {
        truncated_ = true;
        return kNoNode;
    }
    return add_term(parent, code, *delta, factor_permille);
}

NodeIndex Breakdown::add_remainder(NodeIndex parent, std::uint16_t code) noexcept {
    if (size_ == 0 || parent >= size_) {
        truncated_ = true;
        return kNoNode;
    }
    const auto sum = children_sum(parent);
    const auto rest = sum ? checked_sub(nodes_[parent].value, *sum) : std::nullopt;
    if (!rest) {
        truncated_ = true;
        return kNoNode;
    }
    if (*rest == 0) {
        return kNoNode;
    }
    return add_term(parent, code, *rest);
}

void Breakdown::set_total(std::int64_t total) noexcept {
    if (size_ != 0) {
        nodes_[kRootNode].value = total;
    }
}

bool Breakdown::set_total_from_children() noexcept {
    if (size_ == 0) {
        return false;
    }
    const auto sum = children_sum(kRootNode);
    if (!sum) {
        return false;
    }
    nodes_[kRootNode].value = *sum;
    return true;
}

std::size_t Breakdown::depth(NodeIndex i) const noexcept {
    std::size_t levels = 0;
    while (i != kRootNode && i < size_ && levels < kBreakdownCapacity) {
        i = nodes_[i].parent;
        ++levels;
    }
    return levels;
}

std::size_t Breakdown::child_count(NodeIndex parent) const noexcept {
    std::size_t count = 0;
    for (std::size_t i = 1; i < size_; ++i) {
        if (nodes_[i].parent == parent) {
            ++count;
        }
    }
    return count;
}

std::optional<std::int64_t> Breakdown::children_sum(NodeIndex parent) const noexcept {
    std::int64_t sum = 0;
    for (std::size_t i = 1; i < size_; ++i) {
        if (nodes_[i].parent == parent) {
            const auto next = checked_add(sum, nodes_[i].value);
            if (!next) {
                return std::nullopt;
            }
            sum = *next;
        }
    }
    return sum;
}

NodeIndex Breakdown::find(std::uint16_t code) const noexcept {
    for (std::size_t i = 0; i < size_; ++i) {
        if (nodes_[i].code == code) {
            return static_cast<NodeIndex>(i);
        }
    }
    return kNoNode;
}

bool Breakdown::is_consistent() const noexcept {
    if (size_ == 0 || truncated_) {
        return false;
    }
    for (std::size_t i = 0; i < size_; ++i) {
        const auto index = static_cast<NodeIndex>(i);
        if (child_count(index) == 0) {
            continue;
        }
        const auto sum = children_sum(index);
        if (!sum || *sum != nodes_[i].value) {
            return false;
        }
    }
    return true;
}

bool Breakdown::operator==(const Breakdown& other) const noexcept {
    if (size_ != other.size_ || truncated_ != other.truncated_) {
        return false;
    }
    for (std::size_t i = 0; i < size_; ++i) {
        if (!(nodes_[i] == other.nodes_[i])) {
            return false;
        }
    }
    return true;
}

// ---- term codes --------------------------------------------------------------------------------

std::string_view term_key(std::uint16_t code) noexcept {
    const auto it =
        std::lower_bound(kTermKeys.begin(), kTermKeys.end(), code, [](const TermKey& row, std::uint16_t c) {
            return row.code < c;
        });
    if (it == kTermKeys.end() || it->code != code) {
        return {};
    }
    return it->key;
}

std::size_t term_code_count() noexcept {
    return kTermKeys.size();
}

std::uint16_t term_code_at(std::size_t i) noexcept {
    return kTermKeys[i].code;
}

}  // namespace pat::sim::economy
