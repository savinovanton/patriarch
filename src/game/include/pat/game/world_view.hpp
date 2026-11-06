#pragma once

#include "pat/sim/date.hpp"
#include "pat/sim/world.hpp"
#include "pat/ui/character_snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The bridge between the simulation and the UI (E3-T12, ADR-0001): everything here reads an immutable
// World snapshot and produces plain data for the UI (palettes, window snapshots, texts). It never
// changes the world; changes go through commands.
//
// Placeholder content: the simulation core has no names or skills yet, so names are derived from ids
// and skills come from the test generator until the data layer (E4) and the dynasty systems (E7) exist.

namespace pat::game {

/// "12 января 1703" - Julian date with Russian month names (the calendar itself is Julian, GDD).
[[nodiscard]] std::string format_date_ru(sim::Date date);

/// Placeholder clan name for a dynasty ("Шереметевы") and the male surname form ("Шереметев").
[[nodiscard]] std::string dynasty_name(sim::DynastyId dynasty);
[[nodiscard]] std::string dynasty_surname(sim::DynastyId dynasty);

/// RGBA8 colour (red in the lowest byte, alpha 255) of a dynasty on the political map.
[[nodiscard]] std::uint32_t dynasty_color(sim::DynastyId dynasty);
inline constexpr std::uint32_t kUnownedColor = 0xFF707070U;

/// Map palette indexed by map province id (index 0 unused, size map_province_count + 1): the owner's
/// colour. Map province `i` corresponds to simulation province index `i - 1`.
[[nodiscard]] std::vector<std::uint32_t> owner_palette(const sim::World& world,
                                                       std::size_t map_province_count);

/// A value-driven palette (the province's development level, red = low, green = high).
[[nodiscard]] std::vector<std::uint32_t> development_palette(const sim::World& world,
                                                             std::size_t map_province_count);

/// Window data for a character. The character must be alive.
[[nodiscard]] ui::CharacterSnapshot character_snapshot(const sim::World& world, sim::CharacterId character);

/// Head of the dynasty that owns map province `map_province` (invalid if unowned or without a head).
[[nodiscard]] sim::CharacterId head_for_province(const sim::World& world, std::size_t map_province);

/// A living character picked pseudo-randomly from `seed` (invalid if nobody is alive).
[[nodiscard]] sim::CharacterId pick_character(const sim::World& world, std::uint32_t seed);

/// Whole years between two dates (Julian years of 365.25 days).
[[nodiscard]] int age_in_years(sim::Date born, sim::Date today);

}  // namespace pat::game
