#include "pat/data/content_schemas.hpp"

#include "pat/data/demography_schemas.hpp"
#include "pat/data/economy_schemas.hpp"
#include "pat/data/m1_schemas.hpp"
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
    // E7-T09: name and portrait pools (data/names/).
    add_names_schemas(set);
    return set;
}

}  // namespace pat::data
