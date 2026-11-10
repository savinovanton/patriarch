#pragma once

#include "pat/loc/localizer.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Plain data of the heads-up display. The snapshot holds no ready-made interface text: numbers are
// numbers and phrases are string-table keys with a count (docs/dev/localization.md), so the window
// formats everything through the Localizer and can switch language without a new snapshot. Only names
// that come from content (realm, province, city, army, dynasty) are plain text.

namespace pat::ui {

/// One resource counter of the top strip ("Казна 12 480 +84").
struct HudResource {
    std::string key;         // icon and label: treasury, grain, army, faith, influence (ui.res.<key>)
    std::int64_t value = 0;  // shown with group separators
    std::int64_t delta = 0;  // change per period; the sign decides the colour
};

/// How the number of a HudEntry is written.
enum class HudNumber : std::uint8_t {
    Plain,      // "4 800" (ui.fmt.number)
    Thousands,  // "94 т"  (ui.fmt.thousands)
};

/// A row of the right panel: a city, an army or a dynasty with its number ("Москва", "94 т").
struct HudEntry {
    std::string name;  // content name
    std::int64_t value = 0;
    HudNumber format = HudNumber::Plain;
};

/// A card of "Дела государства": something that waits for the player.
struct HudAffair {
    std::string kind;        // icon: bell, charter or embassy
    std::string title_key;   // e.g. "ui.hud.affairs.council.title"
    std::string detail_key;  // e.g. "ui.hud.affairs.council.detail"
    std::int64_t count = 0;  // passed to both messages as {n}
};

/// A line of the chronicle ("11:42", "Посольство прибыло в Москву").
struct HudChronicleLine {
    std::string time;
    std::string key;         // e.g. "ui.hud.chronicle.embassy_arrived"
    std::int64_t count = 0;  // passed to the message as {n}
};

/// Everything the heads-up display shows: plain data, no pointers into the simulation.
struct HudSnapshot {
    std::string realm_name;      // content name, e.g. "Московское царство"
    std::string realm_subtitle;  // "Государев двор" or the selected province
    std::vector<HudResource> resources;
    int bell_count = 0;               // pending court events
    int charter_count = 0;            // pending decrees and letters
    int embassy_count = 0;            // pending embassies
    std::vector<HudEntry> cities;     // tab "Города" of the holdings panel
    std::vector<HudEntry> armies;     // tab "Войска" and the "Войска" block
    std::vector<HudEntry> dynasties;  // tab "Династии"
    std::vector<HudAffair> affairs;
    std::vector<HudChronicleLine> chronicle;  // newest first
};

/// Placeholder content matching the Figma frame, until the economy and court systems exist. The realm
/// name and the card/chronicle texts come from `loc`; city and dynasty names stay placeholder content.
[[nodiscard]] HudSnapshot make_test_hud(const loc::Localizer& loc);

}  // namespace pat::ui
