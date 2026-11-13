#pragma once

#include "pat/game/economy_view.hpp"
#include "pat/ui/province_snapshot.hpp"

namespace pat::game {

/// Selected holding inspector; reads the world without changing simulation state.
[[nodiscard]] ui::ProvinceSnapshot province_snapshot(const loc::Localizer& loc,
                                                     const sim::World& world,
                                                     sim::ProvinceId province,
                                                     const sim::economy::EconomyRules& rules,
                                                     const EconomyViewOptions& options = {});

}  // namespace pat::game
