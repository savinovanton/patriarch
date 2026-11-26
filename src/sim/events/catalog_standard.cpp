#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/character_effects.hpp"
#include "pat/sim/events/character_triggers.hpp"
#include "pat/sim/events/realm_triggers.hpp"
#include "pat/sim/events/state_effects.hpp"

// The one place that lists the registration functions of the catalog blocks; E11-T08 .. T11 add their
// block here (one line each).

namespace pat::sim::events {

Catalog Catalog::standard() {
    Catalog c = engine();
    register_character_triggers(c);
    register_realm_triggers(c);
    register_character_effects(c);
    register_state_effects(c);
    return c;
}

}  // namespace pat::sim::events
