#include "pat/content/session_assembly.hpp"

#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/dynasty_standing.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/mortality.hpp"
#include "pat/sim/opinion.hpp"

#include <format>
#include <utility>

namespace pat::content {

SessionContent SessionContent::demo() {
    SessionContent content;
    content.replay_id = sim::demo::kScenarioId;
    return content;
}

SessionContent SessionContent::from_scenario(const M1Scenario& scenario) {
    SessionContent content;
    content.has_content = true;
    content.demography = scenario.demography;
    content.traits = scenario.traits;
    content.economy_rules = scenario.economy_rules;
    content.replay_id = scenario.replay_id;
    content.content_hash = scenario.content_hash;
    return content;
}

std::string SessionContent::save_error(const sim::World& world, bool ignore_hash) const {
    if (!has_content) {
        return world.economy_present() ? "the demo systems cannot advance an economy save" : std::string{};
    }
    if (!world.economy_present()) {
        return "the save has no M1 economy state";
    }
    const std::uint64_t saved_hash = world.economy().meta.rules_hash;
    if (!ignore_hash && saved_hash != content_hash) {
        return std::format(
            "M1 save content hash mismatch: saved {:016x}, loaded {:016x}", saved_hash, content_hash);
    }
    if (traits) {
        for (const sim::CharacterId id : world.living()) {
            if (sim::check_trait_set(*traits, world.characters().traits[id.index()]) !=
                sim::TraitStatus::Ok) {
                return std::format("M1 save has an incompatible trait set for character {}", id.value());
            }
        }
    }
    return {};
}

sim::Simulation assemble_session(const SessionContent& content,
                                 sim::World world,
                                 std::vector<sim::Command> pending_internal) {
    sim::SimulationConfig config;
    config.traits = content.traits;
    // The systems are installed on an empty placeholder world (they read no world state when installed)
    // and the real state is restored last: the same path for a new game and for a load.
    sim::Simulation simulation(sim::World(sim::Date{}, 0), std::move(config));
    if (content.has_content) {
        simulation.add_system(sim::make_mortality_system(content.demography, content.traits));
        simulation.add_system(sim::make_opinion_system());
        simulation.add_system(sim::make_dynasty_standing_system());
        sim::economy::install_economy_systems(simulation, *content.economy_rules);
    } else {
        sim::demo::install_systems(simulation);
    }
    simulation.restore_state(std::move(world), std::move(pending_internal));
    return simulation;
}

}  // namespace pat::content
