#include "pat/data/content_schemas.hpp"

#include "pat/data/court_schemas.hpp"
#include "pat/data/credits_schemas.hpp"
#include "pat/data/crisis_schemas.hpp"
#include "pat/data/demography_schemas.hpp"
#include "pat/data/economy_schemas.hpp"
#include "pat/data/m1_schemas.hpp"
#include "pat/data/marriage_schemas.hpp"
#include "pat/data/names_schemas.hpp"

namespace pat::data {

SchemaSet content_schemas() {
    SchemaSet set;
    // E4-T05: the definition kinds (provinces, dynasties, traits and skills).
    add_m1_schemas(set);
    // E9-T18: the economy kinds (data/economy/).
    add_economy_schemas(set);
    // E7-T07: the demography balance (data/balance/).
    add_demography_schemas(set);
    // E8-T07: the marriage balance (data/balance/).
    add_marriage_schemas(set);
    // E10-T04: the court kinds (data/court/ and data/balance/court.pd).
    add_court_schemas(set);
    // E12-T04: the crisis kinds (data/balance/crisis.pd and data/crisis/).
    add_crisis_schemas(set);
    // E7-T09: name and portrait pools (data/names/).
    add_names_schemas(set);
    // UI-213: the credits screen entries (data/credits/).
    add_credits_schemas(set);
    return set;
}

}  // namespace pat::data
