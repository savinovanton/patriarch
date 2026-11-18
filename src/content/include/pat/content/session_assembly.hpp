#pragma once

#include "pat/content/m1_scenario.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/demography.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/traits.hpp"
#include "pat/sim/world.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// One assembly of a game session (E13-T07, #372; the shared piece of E30-T10): the systems, the rules and
// the content checks that a Simulation needs, whether it starts a new game, loads a save or runs under
// a tool. simcli and game::load_game both go through here, so "Load" and "New game" cannot drift apart
// (docs/dev/save-format.md 2.4 step 9). Pure data plus functions: no platform, no graphics.

namespace pat::content {

/// What a session is made of besides the World: the loaded content set. Content, not state: nothing here
/// is saved or hashed except content_hash, which a save carries and a load compares.
struct SessionContent {
    /// False for the demo scenario (no content set, no economy): the demo systems, hash 0.
    bool has_content = false;
    sim::DemographyRules demography = sim::kDefaultDemography;
    std::shared_ptr<const sim::TraitCatalog> traits;
    std::shared_ptr<const sim::economy::EconomyRules> economy_rules;
    std::uint64_t replay_id = 0;     ///< the scenario id of replays
    std::uint64_t content_hash = 0;  ///< simulation compatibility hash of the set; 0 = none (demo)

    /// The demo scenario: replay id sim::demo::kScenarioId, no content.
    [[nodiscard]] static SessionContent demo();
    /// The content of a loaded M1 scenario (copies the shared pointers and the rules, not the World).
    [[nodiscard]] static SessionContent from_scenario(const M1Scenario& scenario);

    /// Why a loaded world does not belong to this content set, empty if it does (the economy is on, its
    /// `rules_hash` equals content_hash, every living character's traits are valid). Always empty for
    /// the demo. `ignore_hash` is the `--dev` mode: the content hash is not compared.
    [[nodiscard]] std::string save_error(const sim::World& world, bool ignore_hash = false) const;
};

/// Builds a Simulation for `world` with the systems of this content in their fixed order and, for a
/// loaded save, its pending internal commands. Equivalent to starting from `world` and calling
/// restore_state, which is what a load does.
[[nodiscard]] sim::Simulation assemble_session(const SessionContent& content,
                                               sim::World world,
                                               std::vector<sim::Command> pending_internal = {});

}  // namespace pat::content
