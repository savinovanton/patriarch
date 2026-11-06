#pragma once

#include <string>
#include <vector>

namespace pat::ui {

/// One resource counter of the top strip ("Казна 12 480 +84").
struct HudResource {
    std::string key;    // icon name: treasury, grain, army, faith, influence
    std::string label;  // "Казна"
    std::string value;  // "12 480" (formatted by the game layer)
    std::string delta;  // "+84", "-12" or "" (the sign decides the colour)
};

/// Everything the heads-up display shows: plain data, no pointers into the simulation.
struct HudSnapshot {
    std::string realm_name;      // "Московское царство"
    std::string realm_subtitle;  // "Государев двор"
    std::vector<HudResource> resources;
    int bell_count = 0;     // pending court events
    int charter_count = 0;  // pending decrees and letters
    int embassy_count = 0;  // pending embassies
};

/// Placeholder content matching the Figma frame, until the economy and court systems exist.
[[nodiscard]] HudSnapshot make_test_hud();

}  // namespace pat::ui
