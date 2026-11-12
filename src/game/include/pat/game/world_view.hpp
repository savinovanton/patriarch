#pragma once

#include "pat/loc/localizer.hpp"
#include "pat/sim/date.hpp"
#include "pat/sim/traits.hpp"
#include "pat/sim/world.hpp"
#include "pat/ui/character_snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The bridge between the simulation and the UI (E3-T12, ADR-0001): everything here reads an immutable
// World snapshot and produces plain data for the UI (palettes, window snapshots, texts). It never
// changes the world; changes go through commands.
//
// Demo names are derived from ids. Dynasty skills and traits use the configured simulation catalogue;
// the spike keeps its generated values when no catalogue is supplied.

namespace pat::game {

/// "12 января 1703" - Julian date (the calendar itself is Julian, GDD) with the month name and the
/// layout taken from the string table (ui.date.long, ui.month.*).
[[nodiscard]] std::string format_date(const loc::Localizer& loc, sim::Date date);

/// Placeholder clan name for a dynasty ("Шереметевы") and the male surname form ("Шереметев").
[[nodiscard]] std::string dynasty_name(sim::DynastyId dynasty);
[[nodiscard]] std::string dynasty_surname(sim::DynastyId dynasty);

/// RGBA8 colour (red in the lowest byte, alpha 255) of a dynasty on the political map.
[[nodiscard]] std::uint32_t dynasty_color(sim::DynastyId dynasty);
inline constexpr std::uint32_t kUnownedColor = 0xFF707070U;

/// Map palette indexed by map province id (index 0 unused, size map_province_count + 1): the owner's
/// colour. `map_to_world` is indexed by map province id. An empty mapping uses the demo order,
/// where map province `i` corresponds to simulation province index `i - 1`.
[[nodiscard]] std::vector<std::uint32_t> owner_palette(const sim::World& world,
                                                       std::size_t map_province_count,
                                                       std::span<const sim::ProvinceId> map_to_world = {});

/// A value-driven palette (the province's development level, red = low, green = high).
[[nodiscard]] std::vector<std::uint32_t>
development_palette(const sim::World& world,
                    std::size_t map_province_count,
                    std::span<const sim::ProvinceId> map_to_world = {});

/// Current unrest, green at 0 points and red at 100. Uses the owner palette's map binding contract.
/// Missing economy, unknown provinces and provinces with no people use kUnownedColor.
[[nodiscard]] std::vector<std::uint32_t> unrest_palette(const sim::World& world,
                                                        std::size_t map_province_count,
                                                        std::span<const sim::ProvinceId> map_to_world = {});

/// Population-weighted current mood of nonempty groups, red at -100 points and green at +100.
/// Empty provinces are neutral, even if their empty groups have nonzero stored mood.
[[nodiscard]] std::vector<std::uint32_t> mood_palette(const sim::World& world,
                                                      std::size_t map_province_count,
                                                      std::span<const sim::ProvinceId> map_to_world = {});

/// Window data for a character. The character must be alive. Role, health and the phrases of the
/// family and relation lines come from `loc` (the lines keep the language they were built in).
[[nodiscard]] ui::CharacterSnapshot character_snapshot(const loc::Localizer& loc,
                                                       const sim::World& world,
                                                       sim::CharacterId character,
                                                       const sim::TraitCatalog* traits = nullptr);

/// Localized explanation of a pure simulation query; decimal terms retain the exact Fixed32 value.
[[nodiscard]] ui::Explanation character_explanation(const loc::Localizer& loc,
                                                    const sim::TraitCatalog& catalog,
                                                    const sim::CharacterBreakdown& breakdown,
                                                    std::string_view label_key);

/// Head of the dynasty that owns map province `map_province` (invalid if unowned or without a head).
[[nodiscard]] sim::CharacterId head_for_province(const sim::World& world,
                                                 std::size_t map_province,
                                                 std::span<const sim::ProvinceId> map_to_world = {});

/// A living character picked pseudo-randomly from `seed` (invalid if nobody is alive).
[[nodiscard]] sim::CharacterId pick_character(const sim::World& world, std::uint32_t seed);

/// Whole years between two dates (Julian years of 365.25 days).
[[nodiscard]] int age_in_years(sim::Date born, sim::Date today);

}  // namespace pat::game
