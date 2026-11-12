#pragma once

#include "pat/ui/explanation.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pat::ui {

/// Raw units of an economic value; Points/Pieces use Q16.16. No simulation types cross this boundary.
enum class EconomyUnit : std::uint8_t { Kopecks, People, Points, Pieces, Permille, Ppm, Count };

struct EconomyNumber {
    std::int64_t value = 0;
    EconomyUnit unit = EconomyUnit::Count;
    Explanation explanation;
};

struct EconomyGroupSnapshot {
    std::string key;
    std::string name;
    EconomyNumber population;
    EconomyNumber mood;
    EconomyNumber wealth;
    EconomyNumber projected_mood;
};

struct EconomyBuildingSnapshot {
    std::uint8_t slot = 0;  ///< zero-based identity, not an economic quantity
    std::string key;
    std::string name;
    EconomyNumber upkeep;
};

struct ProvinceEconomySnapshot {
    bool available = false;
    std::uint32_t id = 0;
    std::string name;
    std::string owner;
    bool has_administrator = false;
    std::uint32_t administrator_id = 0;
    std::string administrator;
    std::vector<EconomyGroupSnapshot> groups;
    EconomyNumber population;
    EconomyNumber control;
    EconomyNumber corruption;
    EconomyNumber unrest;
    EconomyNumber grain;
    EconomyNumber slots;
    EconomyNumber empty_slots;
    EconomyNumber income_last_month;
    /// Current-rate queries, distinct from the values already stored in the world.
    EconomyNumber projected_control;
    EconomyNumber projected_corruption;
    EconomyNumber projected_unrest;
    EconomyNumber projected_income;
    std::vector<EconomyBuildingSnapshot> buildings;
};

struct TreasuryLineSnapshot {
    std::uint16_t code = 0;
    std::string name;
    EconomyNumber amount;
};

struct TreasuryMonthSnapshot {
    bool available = false;
    std::string date;
    EconomyNumber opening;
    EconomyNumber net;
    EconomyNumber closing;
    std::vector<TreasuryLineSnapshot> lines;
};

struct TreasurySnapshot {
    bool available = false;
    std::string owner;
    EconomyNumber treasury;
    TreasuryMonthSnapshot current;
    /// Newest closed month first; unavailable entries precede the start of the history.
    std::array<TreasuryMonthSnapshot, 12> history{};
    EconomyNumber projected_monthly_net;
    std::vector<TreasuryLineSnapshot> projected_lines;
    std::array<EconomyNumber, 12> projected_balances{};
};

}  // namespace pat::ui
