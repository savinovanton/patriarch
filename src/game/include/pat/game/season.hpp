#pragma once

#include "pat/sim/date.hpp"
#include "pat/ui/map_overlays.hpp"

// The look of the land through the year (E5-A4, #452): a number per date that the map renderer turns into its
// palette and grading. Owner decision (#447): the map follows the season of the game, winter palette in
// winter and green meadows in summer, and the start (1 January 1703) is winter. Pure functions of a Julian
// date, so a snapshot's `today()` is all that is needed; nothing here touches the world.

namespace pat::game {

/// What the date does to the landscape.
struct SeasonLook {
    float winter =
        1.0f;  ///< 1 = snow-covered winter, 0 = summer green; smooth in between (melt, first snow).
    float autumn = 0.0f;  ///< 0..1 warm golden-brown tint of the summer palette (peaks in early October).
};

/// Share of winter in the picture of `date` (0..1). Full winter lasts from 1 December to 15 March, the thaw
/// runs until 1 May, summer from then until 15 October, the first snow settles by 1 December. The curve is
/// continuous (smoothstep ramps), never leaves 0..1 and has no jump over New Year. The leap day counts as
/// 28 February. (Julian calendar, as everywhere in the simulation.)
[[nodiscard]] float winter_factor(sim::Date date);

/// Autumn tint (0..1): rises from 1 September, peaks around 5 October, gone by 15 November.
[[nodiscard]] float autumn_tint(sim::Date date);

[[nodiscard]] SeasonLook season_look(sim::Date date);

/// A typical date of a season in `year`, handy for screenshots and tests: winter 15 January, spring 10 April
/// (the middle of the thaw), summer 15 July, autumn 5 October.
[[nodiscard]] sim::Date season_sample_date(ui::Season season, int year);

}  // namespace pat::game
