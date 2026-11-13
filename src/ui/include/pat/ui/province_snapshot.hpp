#pragma once

#include "pat/ui/economy_snapshot.hpp"

#include <string>

namespace pat::ui {

/// Presentation of a selected holding. All world queries and economic formatting live in game.
struct ProvinceSnapshot {
    ProvinceEconomySnapshot economy;
    std::string subtitle;
    EconomyNumber income_change;  ///< Current-rate income minus the last recorded monthly income.
    std::string order;
    int order_percent = 0;  ///< Clamped display width of the control track, 0...100.
    std::string garrison;
    Explanation garrison_explanation;
    std::string state;
    std::string state_detail;
    Explanation state_explanation;
    std::string owner_name;
    std::string owner_detail;
    bool owner_portrait = false;  ///< The existing concept portrait is a placeholder for a living head.
    bool manage_available = false;
    Explanation manage_explanation;
};

}  // namespace pat::ui
