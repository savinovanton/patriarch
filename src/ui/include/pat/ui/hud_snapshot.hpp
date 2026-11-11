#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Plain data of the heads-up display (docs/dev/ui-design.md section 2, board 01). The snapshot holds no
// ready-made interface text: numbers are numbers and phrases are string-table keys, so the window formats
// everything through the Localizer and can switch language without a new snapshot. Only names that come
// from content (the house) are plain text.

namespace pat::ui {

/// What the third line of a resource of the top bar says.
enum class HudNote : std::uint8_t {
    Delta,     // the monthly change with a sign: "+42 / мес." (green when positive, red when negative)
    Capacity,  // the limit: "из 1 600"
    Caption,   // a fixed caption of the resource: "при дворе" (ui.hud.res.<key>.note)
};

/// One resource counter of the top bar.
struct HudResource {
    std::string key;         // icon and label: treasury, prestige, influence, piety, militia (ui.res.<key>)
    std::int64_t value = 0;  // shown with group separators; the treasury also as roubles (ui.fmt.rouble)
    std::int64_t delta = 0;  // change per month, for HudNote::Delta
    std::int64_t capacity = 0;  // for HudNote::Capacity
    HudNote note = HudNote::Delta;
};

/// Everything the heads-up display shows: plain data, no pointers into the simulation.
struct HudSnapshot {
    std::string
        house;  // the player's house in the genitive, as content gives it: "Голицыных" ("РОД ГОЛИЦЫНЫХ")
    std::vector<HudResource> resources;
};

/// Placeholder content matching the mockup (board 01), until the economy and court bridge exists (E9-T20):
/// five resources with the numbers of the board and the house of the mockup.
[[nodiscard]] HudSnapshot make_test_hud();

}  // namespace pat::ui
